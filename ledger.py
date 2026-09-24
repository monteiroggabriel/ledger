import argparse
import hashlib
import json
import os
import secrets
from functools import wraps
from time import time
from urllib.parse import urlparse

import requests
from flask import Flask, jsonify, request

CHAIN_FILE    = 'chain.json'
API_KEYS_FILE = 'api_keys.json'


class Ledger(object):
    def __init__(self):
        self.pending_records = []
        self.nodes = set()

        self.chain = self._load_chain()
        if not self.chain:
            self.new_block(previous_hash='0')

    def _load_chain(self):
        if os.path.exists(CHAIN_FILE):
            with open(CHAIN_FILE, 'r') as f:
                return json.load(f)
        return []

    def _save_chain(self):
         with open(CHAIN_FILE, 'w') as f:
            json.dump(self.chain, f, indent=2)

    def register_node(self, address):
        parsed_url = urlparse(address)
        if not parsed_url.netloc:
            raise ValueError(
                f"Invalid node address '{address}'. "
                "Make sure to include the scheme (e.g., 'http://host:port')."
            )
        self.nodes.add(parsed_url.netloc)

    def new_block(self, previous_hash=None):
        block = {
            'index':         len(self.chain) + 1,
            'timestamp':     time(),
            'records':       self.pending_records,
            'previous_hash': previous_hash or self.hash(self.chain[-1]),
        }

        self.pending_records = []
        self.chain.append(block)
        self._save_chain()
        return block

    def submit_record(self, record_id, record_hash, record_type, submitted_by):
        self.pending_records.append({
            'record_id':    record_id,
            'record_hash':  record_hash,
            'record_type':  record_type,
            'submitted_by': submitted_by,
            'submitted_at': time(),
        })

        return self.last_block['index'] + 1

    def find_record(self, record_id):
        matches = []
        for block in self.chain:
            for record in block.get('records', []):
                if record['record_id'] == record_id:
                    matches.append({
                        **record,
                        'block_index':     block['index'],
                        'block_timestamp': block['timestamp'],
                    })
        return matches

    @property
    def last_block(self):
        return self.chain[-1]

    @staticmethod
    def hash(block):
        block_string = json.dumps(block, sort_keys=True).encode()
        return hashlib.sha256(block_string).hexdigest()

    def valid_chain(self, chain):
        last_block = chain[0]
        current_index = 1

        while current_index < len(chain):
            block = chain[current_index]

            if block['previous_hash'] != self.hash(last_block):
                return False

            last_block = block
            current_index += 1

        return True

    def resolve_conflicts(self):
        neighbours = self.nodes
        new_chain = None

        max_length = len(self.chain)

        for node in neighbours:
            try:
                response = requests.get(f'http://{node}/chain', timeout=5)
            except requests.exceptions.RequestException as e:
                print(f'[consensus] Node {node} unreachable, skipping: {e}')
                continue

            if response.status_code == 200:
                data = response.json()
                length = data['length']
                chain = data['chain']

                if length > max_length and self.valid_chain(chain):
                    max_length = length
                    new_chain = chain

        if new_chain:
            self.chain = new_chain
            self._save_chain()
            return True

        return False


def _load_api_keys():
    if os.path.exists(API_KEYS_FILE):
        with open(API_KEYS_FILE, 'r') as f:
            return set(json.load(f))

    bootstrap_key = secrets.token_hex(32)
    _save_api_keys({bootstrap_key})
    print(f'\n[auth] No API keys found. Bootstrap key created:')
    print(f'       {bootstrap_key}')
    print(f'       Store it safely, it will not be shown again.\n')
    return {bootstrap_key}


def _save_api_keys(key_set):
    with open(API_KEYS_FILE, 'w') as f:
        json.dump(list(key_set), f, indent=2)


app = Flask(__name__)
ledger = Ledger()
api_keys = _load_api_keys()


def require_api_key(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        key = request.headers.get('X-API-Key')
        if not key or key not in api_keys:
            return jsonify({'error': 'Unauthorised, valid X-API-Key header required'}), 401
        return f(*args, **kwargs)
    return decorated


@app.route('/blocks/seal', methods=['POST'])
@require_api_key
def seal_block():
    if not ledger.pending_records:
        return jsonify({'error': 'No pending records to seal'}), 400

    block = ledger.new_block()

    response = {
        'message':       'New block sealed',
        'index':         block['index'],
        'records_sealed': len(block['records']),
        'previous_hash': block['previous_hash'],
        'timestamp':     block['timestamp'],
    }

    return jsonify(response), 200


@app.route('/records/submit', methods=['POST'])
@require_api_key
def submit_record():
    values = request.get_json()

    required = ['record_id', 'record_hash', 'record_type', 'submitted_by']
    if not all(k in values for k in required):
        return jsonify({'error': f'Missing fields. Required: {required}'}), 400

    index = ledger.submit_record(
        record_id=values['record_id'],
        record_hash=values['record_hash'],
        record_type=values['record_type'],
        submitted_by=values['submitted_by'],
    )

    response = {
        'message': f'Record queued, will be sealed in block {index}',
        'record_id': values['record_id'],
        'pending_block': index,
    }
    return jsonify(response), 201


@app.route('/records/verify/<record_id>', methods=['GET'])
def verify_record(record_id):
    provided_hash = request.args.get('hash')
    if not provided_hash:
        return jsonify({'error': "Query parameter 'hash' is required"}), 400

    entries = ledger.find_record(record_id)
    if not entries:
        return jsonify({'error': f"No sealed record found for record_id '{record_id}'"}), 404

    latest = entries[-1]
    match = latest['record_hash'] == provided_hash

    response = {
        'record_id':       record_id,
        'valid':           match,
        'stored_hash':     latest['record_hash'],
        'provided_hash':   provided_hash,
        'block_index':     latest['block_index'],
        'block_timestamp': latest['block_timestamp'],
        'submitted_by':    latest['submitted_by'],
    }
    return jsonify(response), 200


@app.route('/records/history/<record_id>', methods=['GET'])
def record_history(record_id):
    entries = ledger.find_record(record_id)
    if not entries:
        return jsonify({'error': f"No sealed record found for record_id '{record_id}'"}), 404

    return jsonify({'record_id': record_id, 'history': entries, 'count': len(entries)}), 200


@app.route('/chain', methods=['GET'])
def full_chain():
    return jsonify({'chain': ledger.chain, 'length': len(ledger.chain)}), 200


@app.route('/chain/validate', methods=['GET'])
def validate_chain():
    chain = ledger.chain

    broken_at = None
    for i in range(1, len(chain)):
        expected = ledger.hash(chain[i - 1])
        if chain[i]['previous_hash'] != expected:
            broken_at = chain[i]['index']
            break

    if broken_at is None:
        return jsonify({
            'valid':        True,
            'blocks_checked': len(chain),
            'message':      'Chain integrity verified, no tampering detected',
        }), 200
    else:
        return jsonify({
            'valid':        False,
            'broken_at_block': broken_at,
            'message':      f'Integrity violation detected at block {broken_at}',
        }), 200


@app.route('/api-keys/create', methods=['POST'])
@require_api_key
def create_api_key():
    new_key = secrets.token_hex(32)
    api_keys.add(new_key)
    _save_api_keys(api_keys)

    label = (request.get_json() or {}).get('label', '')
    return jsonify({
        'message': 'New API key created, store it safely, it will not be shown again',
        'key':     new_key,
        'label':   label,
    }), 201


@app.route('/nodes/register', methods=['POST'])
def register_nodes():
    values = request.get_json()

    nodes = values.get('nodes')
    if not nodes:
        return jsonify({'error': 'Provide a non-empty list of node addresses under "nodes"'}), 400

    errors = []
    for node in nodes:
        try:
            ledger.register_node(node)
        except ValueError as e:
            errors.append(str(e))

    if errors:
        return jsonify({'error': 'Some nodes were rejected', 'details': errors}), 400

    return jsonify({'message': 'Nodes added', 'total_nodes': list(ledger.nodes)}), 201


@app.route('/nodes/resolve', methods=['GET'])
def consensus():
    replaced = ledger.resolve_conflicts()

    if replaced:
        return jsonify({'message': 'Chain was replaced', 'new_chain': ledger.chain}), 200
    return jsonify({'message': 'Chain is authoritative', 'chain': ledger.chain}), 200


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Run an integrity ledger node.')
    parser.add_argument(
        '-p', '--port',
        type=int,
        default=5000,
        help='Port to listen on (default: 5000)',
    )
    args = parser.parse_args()
    app.run(host='0.0.0.0', port=args.port)
