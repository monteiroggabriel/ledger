# Technical Reference

### 1. How the Blockchain Works Here

**Hash chaining:** Each block stores `previous_hash`, the SHA-256 of the previous block, forming a chain:
```
[Genesis Block] -> [Block 2] -> [Block 3] -> ...
previous_hash='0'  previous_hash=hash(B1)  previous_hash=hash(B2)
```

If someone alters any data in Block 2, its hash changes, but Block 3 holds the old hash, so the two no longer match. The chain breaks and the tampering is detectable. This is what `GET /chain/validate` checks: it traverses the blocks verifying whether each `previous_hash` matches the actual hash of the previous one.

It is a permissioned and centralized ledger that uses the blockchain data structure, not a decentralized blockchain like Bitcoin.

| Property | Bitcoin | This ledger |
| --- | --- | --- |
| Hash chaining (tamper-evidence) | ✅ | ✅ |
| Proof of Work | ✅ | ❌ removed |
| Open network | ✅ | ❌ |
| Authorized nodes only | ❌ | ✅ (via API key) |
| Decentralized | ✅ | ❌ centralized |

PoW was removed because its purpose is to prevent anonymous attackers on an open network from creating longer alternative chains. In a closed institutional ledger, the threat model is different: you do not fear malicious forking, you fear silent tampering with records. For this problem, hash chaining is sufficient and PoW is a cost without benefit.

**Normal operation flow:**

```
Researcher calculates SHA-256 of the document
v
POST /records/submit  { record_id, record_hash, record_type, submitted_by }
v
Record enters the pending queue (pending_records)
v
POST /blocks/seal
v
New block created with all pending records, chained to the previous one, saved in chain.json
v
GET /records/verify/<id>?hash=<sha256>  →  { valid: true/false }
```

---

### 2. Data Structure

**Block:** 

```json
{
  "index": 2,
  "timestamp": 1754424000.123,
  "records": [...],
  "previous_hash": "a3f9c2..."
}
```

| Field | Type | Description |
| --- | --- | --- |
| `index` | int | Position in the chain (1-indexed) |
| `timestamp` | float | Unix timestamp at the moment of sealing |
| `records` | list | List of integrity records sealed in this block |
| `previous_hash` | str | SHA-256 of the previous block (ensures chaining) |

The genesis block (index 1) is automatically created on the first execution with `previous_hash='0'` and `records=[]`.

**Integrity record (inside `records`):** 

```json
{
  "record_id":    "dataset-2024-001",
  "record_hash":  "e3b0c44298fc1c149afb...",
  "record_type":  "dataset",
  "submitted_by": "researcher_a",
  "submitted_at": 1754423950.456
}
```

| Field | Type | Description |
| --- | --- | --- |
| `record_id` | str | Unique identifier of the original document |
| `record_hash` | str | Hex SHA-256 of the document, calculated by the client |
| `record_type` | str | Freeform category (e.g., `dataset`, `preprint`, `survey`) |
| `submitted_by` | str | Identifier of the researcher or system that submitted |
| `submitted_at` | float | Unix timestamp of submission |

**Important:** the original document never reaches the ledger. The client calculates the hash locally and sends only the digest. This means sensitive data stays out of the system.

---

### 3. Code Architecture

The code lives entirely in `ledger.py`, a single Python file with an embedded HTTP server via Flask.

**`Ledger` class:**

| Method | What it does |
| --- | --- |
| `__init__` | Initializes `pending_records`, `nodes`, loads the chain from disk or creates the genesis block |
| `_load_chain()` | Reads `chain.json` from disk; returns an empty list if it does not exist |
| `_save_chain()` | Serializes `self.chain` to `chain.json` |
| `register_node(address)` | Adds a peer node (validates that the address has an http:// scheme) |
| `new_block(previous_hash)` | Seals pending records into a block, chains it, saves it |
| `submit_record(...)` | Queues a record into `pending_records` |
| `find_record(record_id)` | Traverses the chain and returns all records with that ID |
| `last_block` | Property: returns `self.chain[-1]` |
| `hash(block)` | Deterministic SHA-256 of a block (`sort_keys=True` ensures consistency) |
| `valid_chain(chain)` | Verifies hash chaining across the entire chain |
| `resolve_conflicts()` | Queries peers, replaces the local chain with the longest valid one |

**Module functions (outside the class):** 

| Function | What it does |
| --- | --- |
| `_load_api_keys()` | Loads keys from `api_keys.json`; generates a bootstrap key on the first execution |
| `_save_api_keys(key_set)` | Persists the key set to disk |
| `require_api_key(f)` | Decorator: blocks write routes without a valid `X-API-Key` header |

---

### 4. API - Complete Reference

**Record routes (write - requires `X-API-Key`):**

`POST /records/submit` - Queues an integrity record.

```bash
curl -X POST http://localhost:5000/records/submit \
  -H "Content-Type: application/json" \
  -H "X-API-Key: <your_key>" \
  -d '{
    "record_id":    "dataset-2024-001",
    "record_hash":  "e3b0c44298fc1c149afb4c8996fb92427ae41e4649b934ca495991b7852b855",
    "record_type":  "dataset",
    "submitted_by": "researcher_a"
  }'

```

Response `201`:
```json
{
  "message": "Record queued, will be sealed in block 2",
  "record_id": "dataset-2024-001",
  "pending_block": 2
}
```

`POST /blocks/seal` - Seals all pending records into a new block (`X-API-Key` header, no body). Returns `200` with `index`, `records_sealed`, `previous_hash`, and `timestamp`; or `400` if there are no pending records.

---

**Verification routes (read - public):**

`GET /records/verify/<record_id>?hash=<sha256>` - Verifies whether a hash matches what is sealed in the chain for that `record_id`. Returns `valid: true/false`, `stored_hash`, `provided_hash`, `block_index`, `block_timestamp`, and `submitted_by`. If `valid` is `false`, the current document does not match the registered one—possible tampering.

`GET /records/history/<record_id>` - Returns all sealed entries for a `record_id` across the entire chain (useful for tracking versions of a document). Response: `count` and a `history` list with `record_hash`, `block_index`, and `submitted_at` for each version.

`GET /chain/validate` - Traverses the entire chain and verifies the integrity of the hash chaining. Returns `{ valid: true, blocks_checked: N }` if intact, or `{ valid: false, broken_at_block: N }` pointing to where the chain broke.

`GET /chain` - Returns the complete chain. Useful for inspection and synchronization between nodes.

---

**Administration routes:**

`POST /api-keys/create` (*requires `X-API-Key*`) - Issues a new API key at runtime.

```bash
curl -X POST http://localhost:5000/api-keys/create \
  -H "Content-Type: application/json" \
  -H "X-API-Key: <existing_key>" \
  -d '{ "label": "sistema-coleta-v2" }'
```

Response `201`:
```json
{
  "message": "New API key created, store it safely, it will not be shown again",
  "key": "4f8a2b...",
  "label": "sistema-coleta-v2"
}
```

> The key is displayed only once. There is no endpoint to retrieve it later.

---

**Network routes (P2P consensus):**

Exist for multi-node scenarios; can be ignored for single-node usage.

`POST /nodes/register` - Registers one or more peer nodes (addresses must include an `http://` scheme).

`GET /nodes/resolve` - Runs the consensus algorithm: replaces the local chain with the longest valid one found among registered peers.

---

### 5. Authentication

The ledger uses **simple API keys via HTTP header** to protect write routes.

| Route | Protected? |
| --- | --- |
| `POST /records/submit` | ✅ |
| `POST /blocks/seal` | ✅ |
| `POST /api-keys/create` | ✅ |
| `GET /records/verify/<id>` | ❌ |
| `GET /records/history/<id>` | ❌ |
| `GET /chain` | ❌ |
| `GET /chain/validate` | ❌ |
| `POST /nodes/register` | ❌ |
| `GET /nodes/resolve` | ❌ |

**First execution:** if `api_keys.json` does not exist, a bootstrap key is automatically generated and printed to the terminal:

```
[auth] No API keys found. Bootstrap key created:
       4f8a2b9c1d3e5f7a...
       Store it safely, it will not be shown again.
```

All protected requests must include the header:

```
X-API-Key: <key>
```

---

### 6. Known Limitations

* **No read authentication:** anyone with network access can read the entire chain. For sensitive data, read routes would also require access control.
* **No key revocation:** there is no endpoint to invalidate an API key. To revoke, manually edit `api_keys.json` and restart the process.
* **Fragile consensus without PoW:** the `resolve_conflicts` algorithm implements longest-chain, but without PoW a node can trivially forge a longer chain. This only matters if you run multiple nodes. For single-node usage, this is not an issue.
* **No pagination on `/chain`:** large chains return everything in a single JSON response.
