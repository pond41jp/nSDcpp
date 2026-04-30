# PostgreSQL Persistence API (`/sdcpp/v1/postgres/*`)

`sd-server` can persist generated image records in PostgreSQL.

> Build requirement: enable with `-DSD_POSTGRESQL=ON`.

## API整理（重複機能の棚卸し）

このAPI群内で似た責務を持つものを整理すると、以下の方針になります。

- `POST /sdcpp/v1/postgres/query` は「検索専用」で維持
  - 理由: 条件検索（タグAND、キーワード、並び替え、同一条件検索、復号パスワード指定）を1つの入口に集約できるため
- `PATCH /sdcpp/v1/postgres/records/{id}` は「部分更新専用」で維持
  - 理由: 検索とは目的が異なり、レコードの任意フィールド更新に特化しているため
- `DELETE /sdcpp/v1/postgres/records/{id}` は「削除専用」で維持
  - 理由: データ削除は更新や検索とは責務が異なるため
- `POST /sdcpp/v1/postgres/config` は「接続設定管理専用」で維持
  - 理由: アプリ設定（`nSDcpp.json`）の更新とDB接続検証の責務を持つため

現時点では、目的が完全に重複しているエンドポイントはないため、削除・統合は行っていません。

## 1. Connection Configuration

### `POST /sdcpp/v1/postgres/config`

Save PostgreSQL settings into `nSDcpp.json`, then validate by opening a DB connection.

Request body:

```json
{
  "host": "127.0.0.1",
  "port": 5432,
  "username": "postgres",
  "password": "postgres",
  "dbname": "nsdcpp",
  "sslmode": "prefer",
  "master_password": "your-master-password"
}
```

Notes:
- `username`, `password`, `dbname` are loaded from `nSDcpp.json` for all DB APIs.
- `master_password` is used to obfuscate per-image passwords before storing.

Response examples:

`200 OK`
```json
{
  "ok": true,
  "host": "127.0.0.1",
  "port": 5432,
  "username": "postgres",
  "dbname": "nsdcpp"
}
```

`400 Bad Request`
```json
{
  "error": "connection failed",
  "message": "FATAL: password authentication failed for user \"postgres\""
}
```

`500 Internal Server Error`
```json
{
  "error": "failed to open nSDcpp.json for write"
}
```

### `GET /sdcpp/v1/postgres/health`

DB疎通とスキーマ利用可否を返します。

Response examples:

`200 OK (healthy)`
```json
{
  "ok": true,
  "connected": true,
  "schema_ready": true,
  "server_version": "PostgreSQL 16.2 on x86_64-pc-linux-gnu ..."
}
```

`200 OK (unhealthy)`
```json
{
  "ok": false,
  "connected": false,
  "schema_ready": false,
  "message": "postgresql username/dbname is not configured"
}
```

### `GET /sdcpp/v1/postgres/schema`

現在のスキーマバージョンと管理テーブル一覧を返します。

Response examples:

`200 OK`
```json
{
  "schema_version": "1",
  "tables": [
    "sdcpp_records",
    "sdcpp_schema_meta",
    "sdcpp_audit_log"
  ]
}
```

`400 Bad Request`
```json
{
  "error": "postgresql username/dbname is not configured"
}
```

### `POST /sdcpp/v1/postgres/schema/migrate`

スキーマ作成/更新を再実行します（冪等）。

Response examples:

`200 OK`
```json
{
  "ok": true,
  "message": "schema migration applied"
}
```

`500 Internal Server Error`
```json
{
  "error": "migration failed",
  "message": "ERROR: permission denied for schema public"
}
```

---

## 2. Save Record API

### `POST /sdcpp/v1/postgres/records`

Store generated image + metadata as one JSON payload.

Request body:

```json
{
  "image_base64": "....",
  "comment": "good sample",
  "rating": 4.5,
  "tags": ["portrait", "anime"],
  "seed": 123456789,
  "prompt": "1girl, cinematic light",
  "negative_prompt": "low quality",
  "width": 832,
  "height": 1216,
  "clip_skip": 2,
  "sampler": "euler_a",
  "scheduler": "discrete",
  "steps": 28,
  "cfg_scale": 7.0,
  "output_format": "png",
  "output_compression": 100,
  "strength": 0.75,
  "batch_count": 1,
  "cache_mode": "disabled",
  "cache_option": "",
  "scm_mask": "",
  "scm_policy_dynamic": true,
  "sample_params": {
    "scheduler": "discrete",
    "sample_method": "euler_a",
    "sample_steps": 28,
    "eta": 1.0,
    "shifted_timestep": 0,
    "flow_shift": 0.0,
    "guidance": {
      "txt_cfg": 7.0,
      "img_cfg": 7.0,
      "distilled_guidance": 3.5
    }
  },
  "high_noise_sample_params": {
    "sample_method": "euler",
    "sample_steps": -1
  },
  "vae_tiling_params": {
    "enabled": false,
    "tile_size_x": 0,
    "tile_size_y": 0
  },
  "hires": {
    "enabled": false,
    "upscaler": "Latent",
    "scale": 2.0,
    "target_width": 0,
    "target_height": 0,
    "steps": 0,
    "denoising_strength": 0.7,
    "upscale_tile_size": 128
  },
  "lora": [],
  "generation_conditions": {
    "prompt": "1girl, cinematic light",
    "negative_prompt": "low quality",
    "width": 832,
    "height": 1216,
    "clip_skip": 2,
    "seed": 123456789,
    "sample_params": {
      "scheduler": "discrete",
      "sample_method": "euler_a",
      "sample_steps": 28
    },
    "output_format": "png",
    "output_compression": 100
  },
  "total_generation_sec": 7.35,
  "tokenizer_sec": 0.05,
  "sampler_total_sec": 6.80,
  "sampler_avg_its": 4.12,
  "decode_sec": 0.50,
  "image_password": "optional-record-password"
}
```

Behavior:
- `seed` tag is always added as `seed:<value>`.
- If user tags are provided, they are stored together with the `seed` tag.
- `generation_conditions` is normalized from request fields, including:
  - `prompt`, `negative_prompt`, `width`, `height`, `clip_skip`, `seed`
  - `sampler`, `scheduler`, `steps`, `cfg_scale`
  - `sample_params`, `high_noise_sample_params`
  - `vae_tiling_params`, `hires`, `lora`
  - `output_format`, `output_compression`
  - `batch_count`, `strength`, `cache_mode`, `cache_option`, `scm_mask`, `scm_policy_dynamic`
- If `image_password` is set:
  - Whole payload JSON is obfuscated.
  - `image_password` itself is obfuscated with `master_password`.
- If `image_password` is empty, payload is stored without obfuscation.

Response examples:

`201 Created`
```json
{
  "id": 12,
  "created_at": "2026-04-29 21:57:40.123+00",
  "encrypted": true,
  "has_password": true
}
```

`400 Bad Request`
```json
{
  "error": "postgresql username/dbname is not configured"
}
```

`500 Internal Server Error`
```json
{
  "error": "insert failed",
  "message": "ERROR: duplicate key value violates unique constraint ..."
}
```

---

## 3. Query API

### `POST /sdcpp/v1/postgres/query`

Supports tag AND search, password-only filter, prompt keyword search, order switching, and same-generation-condition search.

Request body:

```json
{
  "tags_and": ["seed:123456789", "anime"],
  "password_protected_only": true,
  "prompt_keyword": "cinematic",
  "order_by": "generation",
  "order": "desc",
  "same_generation_as_id": 12,
  "limit": 50,
  "passwords": {
    "12": "optional-record-password"
  }
}
```

Filters:
- `tags_and`: AND condition across tags.
- `password_protected_only`: only records with password.
- `prompt_keyword`: keyword search against prompt.
- `same_generation_as_id`: records with same non-prompt generation condition JSON.
- `order_by`: `generation` or `rating`.
- `order`: `asc` or `desc`.

Password-protected records:
- If correct password is provided in `passwords[id]`, decrypted payload is returned.
- Otherwise `payload` is `null` and `error` explains password mismatch.

Response examples:

`200 OK`
```json
{
  "records": [
    {
      "id": 12,
      "created_at": "2026-04-29 21:57:40.123+00",
      "rating": 4.5,
      "tags_text": "{seed:123456789,anime}",
      "prompt": "1girl, cinematic light",
      "encrypted": true,
      "has_password": true,
      "payload": {
        "image_base64": "....",
        "comment": "good sample",
        "seed": 123456789
      }
    },
    {
      "id": 13,
      "created_at": "2026-04-29 22:01:12.234+00",
      "rating": 3.0,
      "tags_text": "{seed:987654321}",
      "prompt": "landscape",
      "encrypted": true,
      "has_password": true,
      "payload": null,
      "error": "password required or invalid"
    }
  ]
}
```

`500 Internal Server Error`
```json
{
  "error": "query failed",
  "message": "ERROR: relation \"sdcpp_records\" does not exist"
}
```

### `GET /sdcpp/v1/postgres/stats`

返却:
- 総レコード数
- 暗号化レコード数
- パスワード保護レコード数
- ユニークタグ数
- payload合計バイト数

Response examples:

`200 OK`
```json
{
  "total_records": 1200,
  "encrypted_records": 350,
  "password_protected_records": 350,
  "unique_tags": 210,
  "payload_total_bytes": 987654321
}
```

### `GET /sdcpp/v1/postgres/tags`

タグ一覧と件数を返します（利用回数順）。

Response examples:

`200 OK`
```json
{
  "tags": [
    { "tag": "seed:123456789", "count": 42 },
    { "tag": "anime", "count": 30 },
    { "tag": "portrait", "count": 12 }
  ]
}
```

### `POST /sdcpp/v1/postgres/tags/rename`

タグ名一括置換。

Request:

```json
{
  "from": "anime",
  "to": "Anime"
}
```

Response examples:

`200 OK`
```json
{
  "ok": true,
  "updated_records": 30
}
```

`400 Bad Request`
```json
{
  "error": "from and to are required"
}
```

---

## 4. Update API

### `PATCH /sdcpp/v1/postgres/records/{id}`

Update arbitrary fields inside stored payload JSON.

Request body example:

```json
{
  "current_password": "old-password-if-encrypted",
  "image_password": "new-password-or-empty-to-clear",
  "comment": "updated comment",
  "rating": 5.0,
  "tags": ["favorite"],
  "generation_conditions": {
    "prompt": "updated prompt"
  }
}
```

Notes:
- For encrypted records, `current_password` is required.
- Any supplied field (except `current_password`) overwrites payload field.
- `seed` tag rule is re-applied on update.
- Setting `image_password` re-encrypts payload with new password.

Response examples:

`200 OK`
```json
{
  "ok": true,
  "id": 12
}
```

`403 Forbidden`
```json
{
  "error": "invalid current_password"
}
```

`404 Not Found`
```json
{
  "error": "record not found"
}
```

---

## 5. Delete API

### `DELETE /sdcpp/v1/postgres/records/{id}`

Delete one record by ID.

Response examples:

`200 OK`
```json
{
  "ok": true,
  "id": 12
}
```

`404 Not Found`
```json
{
  "error": "record not found"
}
```

### `POST /sdcpp/v1/postgres/records/bulk-delete`

条件指定で一括削除します。`dry_run` を推奨。

Request example:

```json
{
  "dry_run": true,
  "ids": [12, 13],
  "tags_and": ["seed:123456789"],
  "older_than_days": 30
}
```

Response examples:

`200 OK (dry run)`
```json
{
  "ok": true,
  "dry_run": true,
  "candidate_records": 24
}
```

`200 OK (executed)`
```json
{
  "ok": true,
  "dry_run": false,
  "deleted_records": 24
}
```

`400 Bad Request`
```json
{
  "error": "at least one delete condition is required"
}
```

### `POST /sdcpp/v1/postgres/records/archive`

古いデータを `sdcpp_records_archive` へ退避後、元テーブルから削除します。`dry_run` を推奨。

Request example:

```json
{
  "dry_run": true,
  "older_than_days": 30
}
```

Response examples:

`200 OK (dry run)`
```json
{
  "ok": true,
  "dry_run": true,
  "candidate_records": 180
}
```

`200 OK (executed)`
```json
{
  "ok": true,
  "dry_run": false,
  "moved_records": 180
}
```

### `POST /sdcpp/v1/postgres/rekey-master-password`

既存 `password_cipher` を新しい `master_password` で再難読化し、`nSDcpp.json` の `master_password` を更新します。

Request example:

```json
{
  "old_master_password": "old-value",
  "new_master_password": "new-value"
}
```

Response examples:

`200 OK`
```json
{
  "ok": true,
  "updated_records": 350
}
```

`403 Forbidden`
```json
{
  "error": "invalid old_master_password"
}
```

### `GET /sdcpp/v1/postgres/audit?limit=100`

監査ログ（作成/検索/更新/削除/一括操作/マイグレーション/リキー）を返します。

Response examples:

`200 OK`
```json
{
  "logs": [
    {
      "id": 101,
      "created_at": "2026-04-30 01:20:11.123+00",
      "action": "record_create",
      "record_id": 12,
      "details": {
        "encrypted": true,
        "has_password": true
      }
    },
    {
      "id": 102,
      "created_at": "2026-04-30 01:21:00.000+00",
      "action": "records_query",
      "record_id": null,
      "details": {
        "result_count": 2
      }
    }
  ]
}
```

---
