-- Rolling back with a portable build still present fails without deleting it.
ALTER TABLE release_artifacts
    DROP CONSTRAINT release_artifacts_kind_check,
    ADD CONSTRAINT release_artifacts_kind_check CHECK (kind IN (
        'windows-exe', 'macos-dmg', 'macos-pkg', 'linux-appimage', 'linux-deb',
        'linux-rpm', 'linux-tar-gz', 'linux-tar-xz'));
