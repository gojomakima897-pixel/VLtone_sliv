CREATE TABLE plugin_pack_versions (
 id uuid PRIMARY KEY,
 pack_id text NOT NULL,
 version text NOT NULL,
 platform text NOT NULL,
 api integer NOT NULL,
 title text NOT NULL,
 description text NOT NULL DEFAULT '',
 download_bytes bigint NOT NULL CHECK (download_bytes > 0),
 installed_bytes bigint NOT NULL CHECK (installed_bytes > 0),
 sha256 text NOT NULL CHECK (length(sha256) = 64),
 created_at timestamptz NOT NULL DEFAULT now(),
 published_at timestamptz,
 hidden boolean NOT NULL DEFAULT false,
 UNIQUE (pack_id, version, platform)
);
CREATE INDEX plugin_pack_catalog ON plugin_pack_versions(platform, pack_id) WHERE published_at IS NOT NULL AND hidden = false;
