use std::os::fd::{FromRawFd, OwnedFd};
use std::time::Duration;
use rmweb_auth_passkey_helper::{ceremony, protocol::{self, ErrorName, Message, Output}};
mod platform;

fn duplicate(fd: i32) -> Option<OwnedFd> {
    let duplicate = unsafe { libc::fcntl(fd, libc::F_DUPFD_CLOEXEC, 3) };
    if duplicate < 0 { None } else { Some(unsafe { OwnedFd::from_raw_fd(duplicate) }) }
}

#[tokio::main(flavor = "multi_thread", worker_threads = 2)]
async fn main() {
    let deadline = tokio::time::Instant::now() + Duration::from_secs(115);
    // Errors and panic payloads are never diagnostics on the helper's pipes.
    std::panic::set_hook(Box::new(|_| {}));
    unsafe {
        libc::umask(0o077);
        if libc::setrlimit(libc::RLIMIT_CORE, &libc::rlimit { rlim_cur: 0, rlim_max: 0 }) != 0 {
            std::process::exit(1);
        }
        let null = libc::open(c"/dev/null".as_ptr(), libc::O_WRONLY | libc::O_CLOEXEC);
        if null < 0 || libc::dup2(null, libc::STDERR_FILENO) != libc::STDERR_FILENO {
            std::process::exit(1);
        }
        if null != libc::STDERR_FILENO { libc::close(null); }
    }
    let Some(input_fd) = duplicate(libc::STDIN_FILENO) else { std::process::exit(1); };
    let Some(output_fd) = duplicate(libc::STDOUT_FILENO) else { std::process::exit(1); };
    // Anonymous pipes only; these APIs set nonblocking mode and do no blocking
    // stdio worker I/O that could outlive the overall deadline.
    let Ok(mut input) = tokio::net::unix::pipe::Receiver::from_owned_fd(input_fd) else { std::process::exit(1); };
    let Ok(output_pipe) = tokio::net::unix::pipe::Sender::from_owned_fd(output_fd) else { std::process::exit(1); };
    let mut output = Output::new(output_pipe);
    let Ok(mut terminate) = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()) else { std::process::exit(1); };
    let Ok(mut interrupt) = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::interrupt()) else { std::process::exit(1); };
    let parent = unsafe { libc::getppid() };
    if parent <= 1 || unsafe { libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM) } != 0 || unsafe { libc::getppid() } != parent {
        let _ = output.send(&Message::Error { name: ErrorName::NotAllowedError }).await;
        std::process::exit(1);
    }
    let validated = {
        let work = async {
            if std::env::args_os().len() != 1 { return Err(ErrorName::OperationError); }
            let bytes = tokio::time::timeout(Duration::from_secs(2), protocol::read_input(&mut input))
                .await.map_err(|_| ErrorName::NotAllowedError)??;
            protocol::prepare(&bytes).await
        };
        tokio::pin!(work);
        tokio::select! {
            biased;
            _ = terminate.recv() => Err(ErrorName::NotAllowedError),
            _ = interrupt.recv() => Err(ErrorName::NotAllowedError),
            result = &mut work => result,
        }
    };
    let mut guard = None;
    let result = match validated {
        Err(error) => Err(error),
        Ok(prepared) => {
            // Keep the original signal receivers: a TERM queued after input
            // validation must still cancel before acquisition touches hardware.
            // Acquire owns partial restoration; never drop it midway.
            match platform::PlatformGuard::acquire(async {
                tokio::select! {
                    biased;
                    _ = terminate.recv() => (),
                    _ = interrupt.recv() => (),
                }
            }).await {
                Err(()) => Err(ErrorName::NotAllowedError),
                Ok(active_guard) => {
                    guard = Some(active_guard);
                    tokio::select! {
                        biased;
                        _ = terminate.recv() => Err(ErrorName::NotAllowedError),
                        _ = interrupt.recv() => Err(ErrorName::NotAllowedError),
                        _ = tokio::time::sleep_until(deadline) => Err(ErrorName::NotAllowedError),
                        result = ceremony::assertion(&prepared, &mut output) => result,
                    }
                }
            }
        }
    };
    // Reserve the final five seconds of the 120-second process budget for
    // platform restoration and a bounded terminal IPC record.
    if let Some(guard) = &mut guard { guard.restore().await; }
    drop(guard);
    let ok = result.is_ok();
    let terminal = result.unwrap_or_else(|name| Message::Error { name });
    let delivered = output.send(&terminal).await.is_ok();
    std::process::exit(if ok && delivered { 0 } else { 1 });
}
