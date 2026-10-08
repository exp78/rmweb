import { randomBytes } from 'node:crypto';
import { generateRegistrationOptions, generateAuthenticationOptions,
    verifyRegistrationResponse, verifyAuthenticationResponse } from '@simplewebauthn/server';

export const BODY_LIMIT = 64 * 1024;
export const RUN_MS = 30 * 60 * 1000;
export class HarnessError extends Error {
    constructor() { super('Test request rejected'); }
}
export function testOrigin(value) {
    try {
        const url = new URL(value);
        if (url.origin !== value || url.username || url.password || url.hash || url.search
            || (url.protocol !== 'https:' && !(url.protocol === 'http:' && url.hostname === 'localhost'))
            || !/^[a-z0-9]+(?:[.-][a-z0-9]+)*$/.test(url.hostname)
            || /^\d+(?:\.\d+)*$/.test(url.hostname) || url.port === '0'
            || (url.protocol === 'https:' && !url.hostname.includes('.'))) throw new HarnessError();
        return url;
    } catch { throw new HarnessError(); }
}
function topLevelClient(response) {
    const encoded = response?.response?.clientDataJSON;
    if (typeof encoded !== 'string' || encoded.length > 8192 || !/^[A-Za-z0-9_-]+$/.test(encoded)) throw new HarnessError();
    const bytes = Buffer.from(encoded, 'base64url');
    if (bytes.toString('base64url') !== encoded) throw new HarnessError();
    const client = JSON.parse(bytes.toString('utf8'));
    if ((client.crossOrigin !== undefined && client.crossOrigin !== false) || 'topOrigin' in client) throw new HarnessError();
}

// Public credential material and single-use challenges only; never a private key.
export class AcceptanceHarness {
    #origin; #rpID; #now; #expires; #sessions = new Map(); #credential;
    #user = randomBytes(32); #verified = false; #busy = false;
    constructor({ origin, now = Date.now }) {
        this.#rpID = testOrigin(origin).hostname;
        this.#origin = origin; this.#now = now; this.#expires = now() + RUN_MS;
    }
    #alive() {
        if (this.#now() >= this.#expires) {
            this.#sessions.clear(); this.#credential = undefined; this.#user.fill(0);
            throw new HarnessError();
        }
    }
    #session(token) {
        this.#alive();
        const session = this.#sessions.get(token);
        if (!session) throw new HarnessError();
        return session;
    }
    openSession() {
        this.#alive();
        if (this.#sessions.size >= 4) throw new HarnessError();
        const token = randomBytes(32).toString('base64url');
        this.#sessions.set(token, { pending: undefined }); return token;
    }
    status(token) {
        this.#session(token);
        return { registered: Boolean(this.#credential), assertionVerified: this.#verified };
    }
    async options(token, kind) {
        try {
            const session = this.#session(token);
            if (this.#busy || session.preparing) throw new HarnessError();
            session.pending = undefined; session.preparing = true;
            let options;
            try {
                if (kind === 'register' && !this.#credential) {
                    options = await generateRegistrationOptions({ rpName: 'Disposable rmweb test',
                        rpID: this.#rpID, userID: this.#user, userName: 'disposable-test',
                        userDisplayName: 'Disposable test', timeout: 120000, attestationType: 'none',
                        supportedAlgorithmIDs: [-7],
                        authenticatorSelection: { residentKey: 'required', userVerification: 'required' } });
                    delete options.extensions;
                } else if (kind === 'authenticate' && this.#credential) {
                    options = await generateAuthenticationOptions({ rpID: this.#rpID, timeout: 120000,
                        userVerification: 'required', allowCredentials: [{ id: this.#credential.id }] });
                } else throw new HarnessError();
                this.#session(token);
                session.pending = { kind, challenge: options.challenge, expires: this.#now() + 120000 };
                return options;
            } finally { session.preparing = false; }
        } catch { throw new HarnessError(); }
    }
    async verify(token, kind, response) {
        let held = false;
        try {
            const session = this.#session(token);
            const pending = session.pending;
            session.pending = undefined;
            if (!pending || pending.kind !== kind || this.#now() >= pending.expires || this.#busy) throw new HarnessError();
            this.#busy = true; held = true;
            if (!response || Buffer.byteLength(JSON.stringify(response)) > BODY_LIMIT) throw new HarnessError();
            topLevelClient(response);
            if (kind === 'register' && !this.#credential) {
                const result = await verifyRegistrationResponse({ response, expectedChallenge: pending.challenge,
                    expectedOrigin: this.#origin, expectedRPID: this.#rpID, supportedAlgorithmIDs: [-7],
                    requireUserPresence: true, requireUserVerification: true });
                this.#session(token);
                if (this.#now() >= pending.expires || !result.verified || !result.registrationInfo || this.#credential) throw new HarnessError();
                this.#credential = result.registrationInfo.credential;
            } else if (kind === 'authenticate' && this.#credential) {
                if (response.id !== this.#credential.id || response.rawId !== this.#credential.id) throw new HarnessError();
                const user = response.response.userHandle;
                if (user != null && user !== this.#user.toString('base64url')) throw new HarnessError();
                const result = await verifyAuthenticationResponse({ response, expectedChallenge: pending.challenge,
                    expectedOrigin: this.#origin, expectedRPID: this.#rpID, credential: this.#credential,
                    requireUserVerification: true });
                this.#session(token);
                if (this.#now() >= pending.expires || !result.verified) throw new HarnessError();
                this.#credential.counter = result.authenticationInfo.newCounter;
                this.#verified = true;
            } else throw new HarnessError();
            return { verified: true };
        } catch { throw new HarnessError(); }
        finally { if (held) this.#busy = false; }
    }
}
