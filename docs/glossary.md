# Security glossary — plain language, A to Z

> Living document. Every term here answers at least one of four questions:
> **is it secret?** (public vs secret) · **is it reversible?** (one-way vs
> two-way) · **is it unique?** (per-user vs shared) · **is it slow on purpose?**
> (fast vs slow). If you can place a term on those axes, it stops being scary.
>
> This is a vocabulary aid, not a proof. It describes what the words mean; the
> implementation and its evidence live in `security/` and the owner tests. Where
> an entry says "not present", that is a stated gap, not a promise.

---

## A

**AEAD** (authenticated encryption with associated data) — encryption that also
proves the ciphertext was not altered. If anyone flips one byte, decryption
refuses. Examples: AES-GCM, ChaCha20-Poly1305. *Analogy:* a locked box with a
tamper seal. *In your tree:* not present yet.

**Argon2id** — the modern password KDF. Slow **and** memory-hungry, so GPUs and
ASICs cannot parallelize guesses cheaply. OWASP minimum: about 19 MiB, 2 passes,
1 lane. *Analogy:* a grinder that fills the room with a huge pile of dough so
only one can run at a time. *In your tree:* not present (needs BLAKE2b).

**Asymmetric encryption** — one key locks, a different key unlocks (public key
locks, private key unlocks). Used for signatures and key exchange, not bulk
data. *In your tree:* not present.

**Authentication tag** — see **Tag**.

---

## B

**base32** — how a TOTP secret is written for `otpauth://` setup URIs so
authenticator apps can read it. *In your tree:* `security/base32.c`.

**bcrypt** — an old password KDF with a 72-byte input limit. Legacy only; do not
choose it for new work. *In your tree:* not present.

**Bearer token** — a token that grants access by possession alone; if stolen, it
works. Different from a *data reference* token, which is inert without the key.

**BLAKE2b** — a modern fast cryptographic hash. Notable because Argon2 is built
on it. *In your tree:* not present.

**Brute force** — trying every possibility.

---

## C

**Checksum** — a fast integrity value (often a CRC) that catches accidental
corruption, not attack. *In your tree:* `util/crc32.c`.

**Ciphertext** — the encrypted form of a value; readable only with the key.

**Client-sealed** — your modifier word for a two-way field whose key the
**client** holds, so the server stores ciphertext it cannot read. *In your tree:*
declared in `oop/modifier.h`; the AEAD behind it is not present yet.

**Constant-time comparison** — comparing secrets without stopping at the first
wrong byte, so timing does not leak how much was guessed right. *In your tree:*
`Crypto_constantTimeEquals`.

**Credential stuffing** — trying passwords leaked from other sites, because
people reuse them.

**Crypto-shredding** — deleting the key so the ciphertext becomes permanently
unreadable. The cheap way to "erase" data.

**CSPRNG** (cryptographically secure random number generator) — the OS's dice,
suitable for keys, salts, tokens, nonces. *In your tree:* `SecureRandom_bytes`.

**Cryptographic hash** — a hash designed to resist attack, unlike a fast symbol
mixer. *In your tree:* `Crypto_sha256` is cryptographic; `Crypto_hash64` is not.

---

## D

**DEK** (data encryption key) — the little key that locks one record or field.

**Dictionary attack** — trying a wordlist of likely passwords.

**Digest** — see **Hash**.

**Domain separation** — deriving different keys for different jobs so one key is
never reused for two purposes. *In your tree:* not present yet (HKDF would).

---

## E

**Encryption** — a locked box: with the key, you get the contents back. Use it
for data you must read back (TOTP seed, email, private notes). *In your tree:*
not present.

**Envelope encryption** — lock each record with its own DEK, then lock all the
DEKs with one KEK. Rotating the KEK re-wraps the little keys instead of
re-encrypting the data. *In your tree:* not present.

**Entropy** — the number of bits of unpredictability in a secret. It is a
property of the **generation process**, not of the string. Hashing or
concatenating cannot add it. A 6-letter lowercase password is about 28 bits; a
128-bit random key is 128 bits.

---

## F

**Fail closed** — if a key or secret store is unavailable, refuse; never fall
back to plaintext. *In your tree:* the Keychain policy already states this.

**Fast hash** — a hash optimized for speed (SHA-256). Great for integrity, **bad
for passwords** because an attacker can try billions per second.

**Fingerprint** — see **Hash**.

**FIPS** — a US government standard that restricts which algorithms may be used;
it is why PBKDF2 still exists. Not a security goal in itself.

**Forward secrecy** — a property where stealing today's key does not decrypt
past traffic. *In your tree:* not claimed.

---

## G

**GCM** — the common AEAD mode for AES (AES-GCM). Reusing its nonce is a
catastrophic bug.

---

## H

**Hash / hash function** — a one-way fingerprint of any input: same input, same
output; cannot go backward. *Analogy:* a meat grinder — a cow becomes mince, and
mince never becomes a cow again. *In your tree:* `Crypto_sha256`.

**HKDF** — a "key-cutting machine": turns one key into many **labeled** subkeys
for different jobs. Adds separation, not entropy. *In your tree:* not present.

**HMAC** — a fingerprint made **with a secret key**: only the key-holder can
produce or verify it. *Analogy:* a wax seal made with a personal signet ring.
*In your tree:* `Hmac_sha256`, `Hmac_sha1`.

**HOTP** — the counter-based ancestor of TOTP.

**HSM** (hardware security module) — hardware that holds a key so it never leaves.
The strongest local option.

---

## I

**Integrity** — proof that data was not altered. A checksum catches accidents; a
MAC or AEAD tag catches attacks.

**IV** (initialization vector) — see **Nonce**.

**Iterations** — how many times a KDF runs. It adds **cost per guess**, not
entropy. *In your tree:* `PASSWORD_ITERATIONS_DEFAULT` (currently 210,000; OWASP
recommends 600,000 for PBKDF2-HMAC-SHA256).

---

## K

**KDF** (key derivation function) — a function that turns something (a password)
into a key, deliberately slowly.

**KEK** (key encryption key) — the master key that locks the little keys (DEKs).

**Key** — the secret that makes encryption or keyed hashing work. If the key
leaks, the protection is gone.

**Key rotation** — replacing a key over time, re-wrapping or re-encrypting the
data it protected.

**Key wrapping** — encrypting a key with another key (a DEK with a KEK).

**Keychain** — the OS's encrypted secret store. *In your tree:* `Keychain_*`;
policy is fail-loud, never a plaintext fallback.

---

## M

**MAC** (message authentication code) — the general name for a keyed integrity
value; HMAC is one kind.

**Memory-hard** — a KDF property (Argon2id, scrypt) that makes guessing expensive
in memory as well as time, which is what defeats GPUs.

**MFA** (multi-factor authentication) — more than one independent factor
(password + TOTP). It adds **independent** entropy, unlike hashing.

---

## N

**Nonce** (number used once; also IV) — a unique value per encryption so the same
plaintext never produces the same ciphertext. Reusing a nonce breaks GCM. Never
reuse.

---

## O

**Offline attack** — you stole the hash or database and guess at home, at your
own speed. Salt, pepper and a slow KDF are the defenses.

**One-way function** — a function that is easy forward and infeasible backward
(a hash). Passwords are protected by these.

**Online attack** — you try logins against the running server. Rate limiting and
lockout are the defenses; hashing does nothing here.

**OWASP** — the community project whose cheat sheets set current parameter
guidance (for example, 600,000 PBKDF2 iterations).

---

## P

**Parallelism** (Argon2's `p`) — how many lanes the memory is divided into. It
does not multiply the memory or act as an iteration count.

**Password hash** — a deliberately slow hash of a password, with a salt, so a
stolen file costs the attacker real time per guess. *In your tree:*
`Password_deriveSha256` (PBKDF2-HMAC-SHA256).

**PBKDF2** — a KDF that runs HMAC many times. Simple, old, FIPS-approved, not
memory-hard.

**Pepper** — a **secret**, **shared** value mixed into the hash and stored
**outside the database** (keychain/HSM/KMS). *Analogy:* a secret ingredient the
chef keeps in a pocket, never in the recipe book. It blocks offline cracking of a
stolen database. *In your tree:* not present — a real gap.

**Plaintext** — the readable, unprotected form of a value.

**PRNG** (pseudo-random number generator) — a predictable shuffle, fine for tests
and animation, **never** for secrets. *In your tree:* `Crypto_prng*`.

**Public key** — the half of an asymmetric key pair that may be shared.

---

## R

**Rainbow table** — a precomputed dictionary of hashes. Defeated by a **salt**.

**Rate limiting** — counting and delaying repeated failures so online guessing
becomes slow. *In your tree:* not present — a stated gap.

**Refresh token** — a long-lived, revocable credential that obtains short-lived
access tokens. *In your tree:* not present.

**Reversible** — two-way: with the key, you get the original back (encryption).

---

## S

**Salt** — a **public**, **unique per user** random value mixed into the hash and
stored beside it. *Analogy:* a serial number stamped on each fingerprint, so two
users with the same password get different fingerprints. It defeats rainbow
tables; it is not secret. *In your tree:* `passwordSalt[16]`.

**scrypt** — a memory-hard KDF; the fallback when Argon2id is unavailable. *In
your tree:* not present.

**Sealed** — your modifier word for a two-way (reversible) protected field.

**Secure Enclave** — Apple hardware that holds a key so it never leaves the chip.

**SecureRandom** — see **CSPRNG**. *In your tree:* `security/secure_random.c`.

**Session token** — a short-lived credential proving an authenticated session,
kept in RAM. *In your tree:* not present (only a process-local boolean).

**SHA-1 / SHA-256** — specific hash designs. SHA-256 is current; SHA-1 is legacy
and should not be chosen for new work. *In your tree:* `security/sha1.c`,
`Crypto_sha256`.

**Shared secret** — a secret known to more than one party; a pepper is shared,
a salt is not secret at all.

**Side channel** — leaking a secret through timing, power, or error behavior
rather than through the algorithm. Constant-time comparison is one defense.

**Symmetric encryption** — one key locks and unlocks (AES, ChaCha20). Used for
bulk data.

---

## T

**Tag** — the tamper seal AEAD produces; verify it and never ignore it.

**Timing attack** — exploiting how long a comparison takes to learn how much was
guessed right.

**Token** — either a **bearer** credential (grants access by possession) or a
**data reference** (inert without the key). The two are not the same thing.

**Tokenization** — replacing the real value with a random reference; the value
itself lives in a separate vault.

**TOTP** — time-based one-time password: six digits from a shared secret plus the
clock. *In your tree:* `Totp_verify`.

**Truncated** — your modifier word for a **lossy** field: the rest is gone
forever, by design.

**Two-way** — see **Reversible**.

---

## W

**WebAuthn / passkeys** — a phishing-resistant authentication factor. *In your
tree:* not present.

**Work factor** — how slow a KDF is (iterations, or memory and passes). It adds
cost per guess, not entropy.

---

## Z

**Zeroization** — overwriting a secret in memory before freeing it, so it does
not linger. *In your tree:* done with `memset` in the password and TOTP paths.

---

## The two contrasts that confuse everyone

**Salt vs pepper**

| | Salt | Pepper |
| :--- | :--- | :--- |
| Secret? | no, public | yes |
| Unique per user? | yes | no, shared |
| Stored with the hash? | yes | no — keychain/HSM |
| Stops | rainbow tables | offline cracking of a stolen DB |
| If it leaks | no big deal | all users exposed; rotate and force resets |

**Hash vs encryption**

| | Hash | Encryption |
| :--- | :--- | :--- |
| Direction | one-way | two-way |
| Key needed? | no key at all | yes, to read |
| Use for | passwords, integrity | TOTP seed, email, private notes |
| If stolen | attacker must guess | attacker needs the key |
| Your word | `hashed` | `sealed` |

---

## Where each thing lives in this tree

- `security/crypto.c` — `Crypto_sha256` (fast hash), `Crypto_constantTimeEquals`,
  `Crypto_prng*` (NOT secure).
- `security/hmac.c` — `Hmac_sha256` (keyed hash).
- `security/password.c` — `Password_deriveSha256` (PBKDF2, slow),
  `Password_generateSalt`.
- `security/secure_random.c` — `SecureRandom_bytes` (the OS dice).
- `security/keychain.c` — the OS secret store (a pepper, a KEK, or a TOTP seed).
- `security/totp.c`, `security/base32.c` — TOTP and its setup encoding.
- `oop/modifier.h` — the words `hashed`, `client-sealed`, `server-sealed`,
  `truncated`.
- **Missing**: a pepper seam, Argon2id, and any AEAD. Those are the three holes
  behind `hashed` and `sealed`.
