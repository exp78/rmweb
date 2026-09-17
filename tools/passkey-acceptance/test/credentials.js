// Test-only generated P-256 keys: cryptographic verifier coverage, not phone proof.
import { generateKeyPairSync, randomBytes, createHash, sign } from 'node:crypto';
import { isoCBOR } from '@simplewebauthn/server/helpers';
export const b64 = bytes => Buffer.from(bytes).toString('base64url');
const hash = bytes => createHash('sha256').update(bytes).digest();
export function testCredential() {
    const { publicKey, privateKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
    const jwk = publicKey.export({ format: 'jwk' });
    const id = randomBytes(24);
    const key = isoCBOR.encode(new Map([[1, 2], [3, -7], [-1, 1],
        [-2, new Uint8Array(Buffer.from(jwk.x, 'base64url'))],
        [-3, new Uint8Array(Buffer.from(jwk.y, 'base64url'))]]));
    function response(options, origin, registration, overrides = {}) {
        const data = Buffer.from(JSON.stringify({ type: registration ? 'webauthn.create' : 'webauthn.get',
            origin, challenge: options.challenge, crossOrigin: false, ...overrides.client }));
        const count = Buffer.alloc(4); count.writeUInt32BE(overrides.counter ?? (registration ? 0 : 1));
        let auth = Buffer.concat([hash(overrides.rpID ?? (registration ? options.rp.id : options.rpId)),
            Buffer.from([overrides.flags ?? (registration ? 0x45 : 0x05)]), count]);
        let result;
        if (registration) {
            const size = Buffer.alloc(2); size.writeUInt16BE(id.length);
            auth = Buffer.concat([auth, Buffer.alloc(16), size, id, key]);
            result = { clientDataJSON: b64(data), attestationObject: b64(isoCBOR.encode(new Map([
                ['fmt', 'none'], ['attStmt', new Map()], ['authData', new Uint8Array(auth)]]))),
                transports: ['internal'] };
        } else {
            const signature = sign('sha256', Buffer.concat([auth, hash(data)]), privateKey);
            if (overrides.badSignature) signature[signature.length - 1] ^= 1;
            result = { clientDataJSON: b64(data), authenticatorData: b64(auth), signature: b64(signature),
                userHandle: overrides.userHandle ?? null };
        }
        return { id: overrides.id ?? b64(id), rawId: overrides.id ?? b64(id), type: 'public-key',
            clientExtensionResults: {}, response: result };
    }
    return { registration: (o, origin, patch) => response(o, origin, true, patch),
        assertion: (o, origin, patch) => response(o, origin, false, patch) };
}
