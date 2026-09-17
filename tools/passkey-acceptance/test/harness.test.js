import test from 'node:test';
import assert from 'node:assert/strict';
import { AcceptanceHarness, HarnessError } from '../harness.js';
import { testCredential } from './credentials.js';
const origin = 'https://passkey.example.test';
function setup() {
    let time = 1000;
    const harness = new AcceptanceHarness({ origin, now: () => time });
    const session = harness.openSession();
    return { harness, session, clock: n => { time += n; } };
}
async function registered() {
    const fixture = setup(); const key = testCredential();
    const options = await fixture.harness.options(fixture.session, 'register');
    assert.deepEqual(await fixture.harness.verify(fixture.session, 'register', key.registration(options, origin)), { verified: true });
    return { ...fixture, key };
}
const rejected = promise => assert.rejects(promise, HarnessError);

test('synthetic registration and a fresh cryptographically signed assertion verify', async () => {
    const { harness, session, key } = await registered();
    const options = await harness.options(session, 'authenticate');
    assert.equal(options.userVerification, 'required');
    assert.equal(options.allowCredentials.length, 1);
    assert.equal(options.extensions, undefined);
    assert.deepEqual(await harness.verify(session, 'authenticate', key.assertion(options, origin)), { verified: true });
    assert.equal(harness.status(session).assertionVerified, true);
});
for (const [name, change] of Object.entries({
    signature: { badSignature: true }, origin: { client: { origin: 'https://other.example.test' } },
    challenge: { client: { challenge: 'not-the-challenge' } }, RP: { rpID: 'other.example.test' },
    UV: { flags: 1 }, UP: { flags: 4 }, type: { client: { type: 'webauthn.create' } },
    crossOrigin: { client: { crossOrigin: true } }, topOrigin: { client: { topOrigin: origin } },
    credential: { id: 'AQID' }, userHandle: { userHandle: 'AQID' },
})) test(`rejects a genuinely signed assertion with wrong ${name}`, async () => {
    const { harness, session, key } = await registered();
    const options = await harness.options(session, 'authenticate');
    await rejected(harness.verify(session, 'authenticate', key.assertion(options, origin, change)));
    assert.equal(harness.status(session).assertionVerified, false);
    await rejected(harness.verify(session, 'authenticate', key.assertion(options, origin)));
});
test('successful assertion cannot replay and new challenges require a new signature', async () => {
    const { harness, session, key } = await registered();
    const one = await harness.options(session, 'authenticate'); const proof = key.assertion(one, origin);
    await harness.verify(session, 'authenticate', proof);
    await rejected(harness.verify(session, 'authenticate', proof));
    const two = await harness.options(session, 'authenticate');
    assert.notEqual(one.challenge, two.challenge);
    await rejected(harness.verify(session, 'authenticate', proof));
});
test('signature counter is updated after success', async () => {
    const { harness, session, key } = await registered();
    await harness.verify(session, 'authenticate', key.assertion(await harness.options(session, 'authenticate'), origin));
    await rejected(harness.verify(session, 'authenticate', key.assertion(await harness.options(session, 'authenticate'), origin)));
    await harness.verify(session, 'authenticate', key.assertion(await harness.options(session, 'authenticate'), origin, { counter: 2 }));
});
test('expired challenge and expired run fail closed', async () => {
    const { harness, session, key, clock } = await registered();
    const proof = key.assertion(await harness.options(session, 'authenticate'), origin);
    clock(120001); await rejected(harness.verify(session, 'authenticate', proof));
    clock(30 * 60 * 1000); assert.throws(() => harness.status(session), HarnessError);
});
test('memory is bounded and registration cannot be replaced', async () => {
    const { harness, session, key } = await registered();
    for (let i = 1; i < 4; i++) harness.openSession();
    assert.throws(() => harness.openSession(), HarnessError);
    await rejected(harness.options(session, 'register'));
    await harness.verify(session, 'authenticate', key.assertion(await harness.options(session, 'authenticate'), origin));
});
test('unregistered, cross-session, malformed and oversized proofs are rejected', async () => {
    const { harness, session } = setup();
    await rejected(harness.options(session, 'authenticate'));
    const key = testCredential(); const options = await harness.options(session, 'register');
    await rejected(harness.verify(harness.openSession(), 'register', key.registration(options, origin)));
    await rejected(harness.verify(session, 'register', {}));
    await harness.options(session, 'register');
    await rejected(harness.verify(session, 'register', { id: 'x'.repeat(65537) }));
});
for (const [name, change] of Object.entries({ origin: { client: { origin: 'https://other.example.test' } },
    RP: { rpID: 'other.example.test' }, UV: { flags: 0x41 }, crossOrigin: { client: { crossOrigin: true } } })) {
    test(`registration rejects wrong ${name}`, async () => {
        const { harness, session } = setup(); const key = testCredential();
        const options = await harness.options(session, 'register');
        await rejected(harness.verify(session, 'register', key.registration(options, origin, change)));
        assert.equal(harness.status(session).registered, false);
    });
}
