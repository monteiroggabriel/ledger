# Referência Técnica

### 1. Como a Blockchain Funciona Aqui

**Hash encadeado:** Cada bloco guarda `previous_hash`, o SHA-256 do bloco anterior, formando uma corrente:
```
[Bloco Gênese] -> [Bloco 2] -> [Bloco 3] -> ...
previous_hash='0'  previous_hash=hash(B1)  previous_hash=hash(B2)
```

Se alguém alterar qualquer dado do Bloco 2, seu hash muda, mas o Bloco 3 guarda o hash antigo, então os dois deixam de bater. A corrente quebra e a adulteração é detectável. Isso é o que `GET /chain/validate` verifica: percorre os blocos conferindo se cada `previous_hash` bate com o hash real do anterior.

É um ledger permissionado e centralizado que usa a estrutura de dados blockchain, não uma blockchain descentralizada como o Bitcoin.

| Propriedade | Bitcoin | Este ledger |
| - | - | - |
| Hash encadeado (tamper-evidence) | ✅ | ✅ |
| Proof of Work | ✅ | ❌ removido |
| Rede aberta | ✅ | ❌ |
| Nós autorizados apenas | ❌ | ✅ (via API key) |
| Descentralizado | ✅ | ❌ centralizado |

O PoW foi removido porque seu propósito é impedir que atacantes anônimos em uma rede aberta criem cadeias alternativas mais longas. Em um ledger institucional fechado, o modelo de ameaça é diferente: você não teme forking malicioso, você teme adulteração silenciosa de registros. Para esse problema, o hash encadeado é suficiente e o PoW é custo sem benefício.

**Fluxo de operação normal:**

```
Pesquisador calcula SHA-256 do documento
v
POST /records/submit  { record_id, record_hash, record_type, submitted_by }
v
Registro entra na fila pendente (pending_records)
v
POST /blocks/seal
v
Novo bloco criado com todos os pendentes, encadeado ao anterior, salvo em chain.json
v
GET /records/verify/<id>?hash=<sha256>  →  { valid: true/false }
```

---

### 2. Estrutura dos Dados

**Bloco:**  

```json
{
  "index": 2,
  "timestamp": 1754424000.123,
  "records": [...],
  "previous_hash": "a3f9c2..."
}
```

| Campo | Tipo | Descrição |
|---|---|---|
| `index` | int | Posição na chain (1-indexed) |
| `timestamp` | float | Unix timestamp do momento do selo |
| `records` | list | Lista de registros de integridade selados neste bloco |
| `previous_hash` | str | SHA-256 do bloco anterior (garante o encadeamento) |

O bloco gênese (índice 1) é criado automaticamente na primeira execução com `previous_hash='0'` e `records=[]`.

**Registro de integridade (dentro de `records`):**  

```json
{
  "record_id":    "dataset-2024-001",
  "record_hash":  "e3b0c44298fc1c149afb...",
  "record_type":  "dataset",
  "submitted_by": "researcher_a",
  "submitted_at": 1754423950.456
}
```

| Campo | Tipo | Descrição |
|---|---|---|
| `record_id` | str | Identificador único do documento original |
| `record_hash` | str | SHA-256 hex do documento, calculado pelo cliente |
| `record_type` | str | Categoria livre (ex: `dataset`, `preprint`, `survey`) |
| `submitted_by` | str | Identificador do pesquisador ou sistema que submeteu |
| `submitted_at` | float | Unix timestamp da submissão |

**Importante:** o documento original nunca chega ao ledger. O cliente calcula o hash localmente e envia apenas o digest. Isso significa que dados sensíveis ficam fora do sistema.

---

### 3. Arquitetura do Código

O código vive inteiro em `ledger.py`, um único arquivo Python com servidor HTTP embutido via Flask.

**Classe `Ledger`:** 

| Método | O que faz |
| - | - |
| `__init__` | Inicializa `pending_records`, `nodes`, carrega a chain do disco ou cria o bloco gênese |
| `_load_chain()` | Lê `chain.json` do disco; retorna lista vazia se não existe |
| `_save_chain()` | Serializa `self.chain` para `chain.json` |
| `register_node(address)` | Adiciona um nó peer (valida que o endereço tem esquema http://) |
| `new_block(previous_hash)` | Sela os registros pendentes num bloco, encadeia, salva |
| `submit_record(...)` | Enfileira um registro em `pending_records` |
| `find_record(record_id)` | Percorre a chain e retorna todos os registros com aquele ID |
| `last_block` | Property: retorna `self.chain[-1]` |
| `hash(block)` | SHA-256 determinístico de um bloco (`sort_keys=True` garante consistência) |
| `valid_chain(chain)` | Verifica encadeamento de hash de toda a chain |
| `resolve_conflicts()` | Consulta peers, substitui a chain local pela mais longa e válida |

**Funções de módulo (fora da classe):**  

| Função | O que faz |
| - | - |
| `_load_api_keys()` | Carrega chaves de `api_keys.json`; gera bootstrap key na primeira execução |
| `_save_api_keys(key_set)` | Persiste o conjunto de chaves no disco |
| `require_api_key(f)` | Decorator: bloqueia rotas de escrita sem header `X-API-Key` válido |

---

### 4. API - Referência Completa

**Rotas de registros (escrita - requer `X-API-Key`):**

`POST /records/submit` - Enfileira um registro de integridade.

```bash
curl -X POST http://localhost:5000/records/submit \
  -H "Content-Type: application/json" \
  -H "X-API-Key: <sua_chave>" \
  -d '{
    "record_id":    "dataset-2024-001",
    "record_hash":  "e3b0c44298fc1c149afb4c8996fb92427ae41e4649b934ca495991b7852b855",
    "record_type":  "dataset",
    "submitted_by": "researcher_a"
  }'
```

Resposta `201`:
```json
{
  "message": "Record queued, will be sealed in block 2",
  "record_id": "dataset-2024-001",
  "pending_block": 2
}
```

`POST /blocks/seal` - Sela todos os registros pendentes em um novo bloco (header `X-API-Key`, sem corpo). Retorna `200` com `index`, `records_sealed`, `previous_hash` e `timestamp`; ou `400` se não há registros pendentes.

---

**Rotas de verificação (leitura - públicas):**

`GET /records/verify/<record_id>?hash=<sha256>` - Verifica se um hash bate com o que está selado na chain para aquele `record_id`. Retorna `valid: true/false`, `stored_hash`, `provided_hash`, `block_index`, `block_timestamp` e `submitted_by`. Se `valid` for `false`, o documento atual não bate com o registrado, possível adulteração.

`GET /records/history/<record_id>` - Retorna todas as entradas seladas para um `record_id` ao longo de toda a chain (útil para rastrear versões de um documento). Resposta: `count` e lista `history` com `record_hash`, `block_index` e `submitted_at` de cada versão.

`GET /chain/validate` - Percorre toda a chain e verifica a integridade do encadeamento de hashes. Retorna `{ valid: true, blocks_checked: N }` se íntegro, ou `{ valid: false, broken_at_block: N }` apontando onde a corrente quebrou.

`GET /chain` - Retorna a chain completa. Útil para inspeção e sincronização entre nós.

---

**Rotas de administração:**

`POST /api-keys/create` (*requer `X-API-Key`*) - Emite uma nova chave de API em runtime.

```bash
curl -X POST http://localhost:5000/api-keys/create \
  -H "Content-Type: application/json" \
  -H "X-API-Key: <chave_existente>" \
  -d '{ "label": "sistema-coleta-v2" }'
```

Resposta `201`:
```json
{
  "message": "New API key created, store it safely, it will not be shown again",
  "key": "4f8a2b...",
  "label": "sistema-coleta-v2"
}
```

> A chave é exibida uma única vez. Não há endpoint para recuperá-la depois.

---

**Rotas de rede (consenso P2P):**

Existem para o cenário de múltiplos nós, podem ser ignoradas para uso com nó único.

`POST /nodes/register` - registra um ou mais nós peers (endereços devem incluir esquema `http://`).

`GET /nodes/resolve` - executa o algoritmo de consenso: substitui a chain local pela mais longa e válida encontrada nos peers registrados.

---

### 5. Autenticação

O ledger usa **API keys simples via header HTTP** para proteger rotas de escrita.

| Rota | Protegida? |
| - | - |
| `POST /records/submit` | ✅ |
| `POST /blocks/seal` | ✅ |
| `POST /api-keys/create` | ✅ |
| `GET /records/verify/<id>` | ❌ |
| `GET /records/history/<id>` | ❌ |
| `GET /chain` | ❌ |
| `GET /chain/validate` | ❌ |
| `POST /nodes/register` | ❌ |
| `GET /nodes/resolve` | ❌ |

**Primeira execução:** se `api_keys.json` não existir, uma bootstrap key é gerada automaticamente e impressa no terminal:

```
[auth] No API keys found. Bootstrap key created:
       4f8a2b9c1d3e5f7a...
       Store it safely, it will not be shown again.
```

Todas as requisições protegidas devem incluir o header:

```
X-API-Key: <chave>
```

---

### 6. Limitações Conhecidas

* **Sem autenticação na leitura:** qualquer um com acesso à rede pode ler toda a chain. Para dados sensíveis, as rotas de leitura também precisariam de controle de acesso.
* **Sem revogação de chaves:** não há endpoint para invalidar uma API key. Para revogar, edite `api_keys.json` manualmente e reinicie o processo.
* **Consenso frágil sem PoW:** o algoritmo `resolve_conflicts` implementa longest-chain, mas sem PoW um nó pode trivialmente forjar uma chain mais longa. Isso só importa se você executar múltiplos nós. Para uso com nó único, não é um problema.
* **Sem paginação em `/chain`:** chains grandes retornam tudo em uma única resposta JSON.
