# PostgreSQL Persistence API (`/sdcpp/v1/postgres/*`)

`sd-server` can persist generated image records in PostgreSQL.

> Build requirement: enable with `-DSD_POSTGRESQL=ON`.

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

Response:

```json
{
  "id": 12,
  "created_at": "2026-04-29 21:57:40.123+00",
  "encrypted": true,
  "has_password": true
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

---

## 5. Delete API

### `DELETE /sdcpp/v1/postgres/records/{id}`

Delete one record by ID.

Response:

```json
{
  "ok": true,
  "id": 12
}
```

