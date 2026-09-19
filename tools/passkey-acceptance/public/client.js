'use strict';
// Remove the transient join code before touching the page or starting requests.
let initialCode = '';
if (location.hash) {
    const fragment = location.hash;
    history.replaceState(null, '', location.pathname + location.search);
    const match = /^#join=([A-Za-z0-9_-]{32,128})$/.exec(fragment);
    if (match) initialCode = match[1];
}
const registration = location.pathname === '/register';
const kind = registration ? 'register' : 'authenticate';
const status = document.getElementById('status');
const stateTitle = document.getElementById('state-title');
const result = document.getElementById('result');
const perform = document.getElementById('perform');
const join = document.getElementById('join');
const code = document.getElementById('code');
const joinControls = document.getElementById('join-controls');
const check = document.getElementById('check');
const steps = ['step-setup', 'step-approval', 'step-verified'].map(id => document.getElementById(id));
const POLL_INTERVAL_MS = 2000;
const MAX_SETUP_CHECKS = 90;
let busy = false, closed = false, checking = false, pollTimer, setupChecks = 0, epoch = 0;
const controllers = new Set();
const current = value => !closed && value === epoch;

document.getElementById('purpose').textContent = registration
    ? 'First, create one disposable passkey on this phone. Then return to the tablet.'
    : 'Create the test passkey on your phone, then use it to approve a request from this tablet.';
perform.textContent = registration ? 'Create test passkey' : 'Test phone passkey';
function show(title, message, stage, outcome = 'waiting') {
    stateTitle.textContent = title;
    status.textContent = message;
    result.dataset.outcome = outcome;
    steps.forEach((step, index) => {
        step.dataset.state = index < stage ? 'complete' : index === stage ? 'current' : 'pending';
        if (index === stage) step.setAttribute('aria-current', 'step');
        else step.removeAttribute('aria-current');
    });
}
function stopPolling() {
    if (pollTimer !== undefined) clearTimeout(pollTimer);
    pollTimer = undefined;
}
function ready() {
    stopPolling(); check.hidden = true;
    perform.hidden = registration;
    show('Phone is ready', registration
        ? 'Return to the tablet and tap Test phone passkey.'
        : 'Tap Test phone passkey, scan the code that appears, and approve on your phone.', 1, 'ready');
}
const decode = value => Uint8Array.from(atob(value.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - value.length % 4) % 4)), char => char.charCodeAt(0));
const encode = value => btoa(String.fromCharCode(...new Uint8Array(value))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
async function request(path, value) {
    const requestEpoch = epoch;
    const controller = new AbortController();
    controllers.add(controller);
    const timeout = setTimeout(() => controller.abort(), 10000);
    try {
        const response = await fetch(path, { method: 'POST', headers: { 'Content-Type': 'application/json' },
            credentials: 'same-origin', cache: 'no-store', signal: controller.signal, body: JSON.stringify(value) });
        if (!response.ok) throw new Error('Rejected');
        const responseValue = await response.json();
        if (!current(requestEpoch)) throw new Error('Inactive page');
        return responseValue;
    } finally { clearTimeout(timeout); controllers.delete(controller); }
}
async function checkSetup() {
    pollTimer = undefined;
    if (closed || checking || busy) return;
    const operationEpoch = epoch;
    checking = true; check.disabled = true; setupChecks++;
    try {
        const state = await request('/api/status', {});
        if (!current(operationEpoch)) return;
        if (state.registered) { ready(); return; }
        if (setupChecks >= MAX_SETUP_CHECKS) {
            show('Still waiting for your phone', 'Finish creating the passkey on your phone, then tap Check again.', 0);
            check.hidden = false;
        } else pollTimer = setTimeout(checkSetup, POLL_INTERVAL_MS);
    } catch {
        if (current(operationEpoch)) {
            show('Could not check phone setup', 'Check the connection, then tap Check again. If the test ended, reopen the setup links for a new run.', 0, 'error');
            check.hidden = false;
        }
    } finally {
        if (current(operationEpoch)) { checking = false; check.disabled = false; }
    }
}
function waitForPhone() {
    stopPolling(); setupChecks = 0; perform.hidden = true; check.hidden = true;
    show('Set up your phone', 'Open the phone setup link and create a test passkey. This screen will update when it is ready.', 0);
    pollTimer = setTimeout(checkSetup, POLL_INTERVAL_MS);
}
function showJoined(state) {
    code.value = ''; joinControls.hidden = true;
    if (state.registered) ready();
    else if (registration) {
        perform.hidden = false;
        show('Create a test passkey', 'Tap Create test passkey and approve the request on this phone.', 0, 'ready');
    } else waitForPhone();
}
async function joinTest(value) {
    if (busy || closed) return;
    const operationEpoch = epoch;
    busy = true; join.disabled = true;
    show('Joining test…', 'Connecting to this disposable test.', 0);
    try {
        const state = await request('/api/join', { code: value });
        if (!current(operationEpoch)) return;
        showJoined(state);
    } catch {
        if (current(operationEpoch)) {
            joinControls.hidden = false;
            show('Could not join this test', 'Check the connection and access code, or reopen the setup link for the current run.', 0, 'error');
        }
    } finally {
        if (current(operationEpoch)) { busy = false; join.disabled = false; }
    }
}
async function resumeSession() {
    const operationEpoch = ++epoch;
    closed = false; busy = true; checking = false; stopPolling();
    join.disabled = true; perform.disabled = true; check.hidden = true;
    show('Resuming test…', 'Checking this test session.', 0);
    try {
        const state = await request('/api/status', {});
        if (current(operationEpoch)) showJoined(state);
    } catch {
        if (current(operationEpoch)) {
            perform.hidden = true; joinControls.hidden = false;
            show('Rejoin this test', 'Reopen the setup link for the current run, or enter its access code below.', 0, 'error');
        }
    } finally {
        if (current(operationEpoch)) { busy = false; join.disabled = false; perform.disabled = false; }
    }
}
join.addEventListener('click', () => joinTest(code.value));
check.addEventListener('click', () => {
    if (busy || checking || closed) return;
    waitForPhone();
});
perform.addEventListener('click', async () => {
    if (busy || closed) return;
    const operationEpoch = epoch;
    busy = true; perform.disabled = true; stopPolling(); check.hidden = true;
    let phase = 'options';
    try {
        show(registration ? 'Creating your passkey…' : 'Waiting for your phone…', registration
            ? 'Approve the passkey request on this phone.'
            : 'Scan the code on this tablet, then approve on your phone. Keep Bluetooth on.', registration ? 0 : 1);
        const options = await request('/api/options', { kind });
        if (!current(operationEpoch)) return;
        options.challenge = decode(options.challenge);
        if (registration) options.user.id = decode(options.user.id);
        for (const descriptor of options.allowCredentials || options.excludeCredentials || []) descriptor.id = decode(descriptor.id);
        phase = 'phone';
        const controller = new AbortController();
        controllers.add(controller);
        let credential;
        try {
            credential = registration ? await navigator.credentials.create({ publicKey: options, signal: controller.signal })
                : await navigator.credentials.get({ publicKey: options, signal: controller.signal });
        } finally { controllers.delete(controller); }
        if (!current(operationEpoch)) return;
        const proof = { id: credential.id, rawId: encode(credential.rawId), type: credential.type,
            clientExtensionResults: credential.getClientExtensionResults(), response: { clientDataJSON: encode(credential.response.clientDataJSON) } };
        if (registration) {
            proof.response.attestationObject = encode(credential.response.attestationObject);
            proof.response.transports = credential.response.getTransports ? credential.response.getTransports() : [];
        } else {
            proof.response.authenticatorData = encode(credential.response.authenticatorData);
            proof.response.signature = encode(credential.response.signature);
            proof.response.userHandle = credential.response.userHandle ? encode(credential.response.userHandle) : null;
        }
        phase = 'verify';
        show('Checking your passkey…', 'Waiting for the test server to check this attempt.', registration ? 0 : 2);
        const verified = await request('/api/verify', { kind, credential: proof });
        if (!current(operationEpoch)) return;
        if (verified.verified !== true) throw new Error('Not verified');
        if (registration) ready();
        else show('PASS', 'Your phone passkey worked. Its signature, challenge, site, presence and verification were checked.', 3, 'pass');
    } catch (error) {
        if (!current(operationEpoch)) return;
        if (phase === 'phone' && (error.name === 'NotAllowedError' || error.name === 'AbortError'))
            show('Not completed', 'The request was cancelled or timed out. You can try again.', registration ? 0 : 1);
        else if (phase === 'verify')
            show('FAIL — not verified', 'This attempt was not verified. Check the connection and try again with a fresh request.', registration ? 0 : 1, 'error');
        else show('Could not complete the test', 'Check the connection and phone setup, then try again. If the test ended, reopen the setup links for a new run.', registration ? 0 : 1, 'error');
    } finally {
        if (current(operationEpoch)) { busy = false; perform.disabled = false; }
    }
});
addEventListener('pagehide', () => {
    closed = true; epoch++; stopPolling();
    for (const controller of controllers) controller.abort();
    controllers.clear();
});
addEventListener('pageshow', event => { if (event.persisted) return resumeSession(); });
if (initialCode) {
    joinTest(initialCode);
    initialCode = '';
}
