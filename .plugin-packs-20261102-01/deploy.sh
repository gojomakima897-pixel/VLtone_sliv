#!/usr/bin/env bash
set -euo pipefail
umask 027
base=/opt/vlt-account-platform
stage="$base/.plugin-packs-20261102-01"
release="$base/releases/plugin-packs-20261102"
previous=$(readlink -f "$base/current")
[[ "$previous" == "$base/releases/"* && -x "$previous/bin/vlt-api" ]]
[[ ! -e "$release" ]]
test -s "$stage/vlt-api"
test -s "$stage/admin-runtime.tar.gz"
install -d -m 0755 "$release"
cp -a "$previous/." "$release/"
install -m 0755 "$stage/vlt-api" "$release/bin/vlt-api"
install -m 0755 "$stage/vlt-migrate" "$release/bin/vlt-migrate"
[[ "$release/admin/admin/.next" == "$base/releases/plugin-packs-20261102/admin/admin/.next" ]]
rm -rf -- "$release/admin/admin/.next"
tar -xzf "$stage/admin-runtime.tar.gz" -C "$release/admin/admin"
install -m 0755 "$stage/backup.sh" "$release/ops/backup.sh"
chown -R vltaccount:vltaccount "$release"
node --check "$release/admin/admin/server.js"
set -a
. /etc/vlt-account/api.env
set +a
pg_dump "$DATABASE_URL" --format=custom --compress=3 --no-owner --file="$stage/database-before.dump"
chmod 0600 "$stage/database-before.dump"
"$release/bin/vlt-migrate" up
printf '%s\n' "$previous" > "$stage/previous-release"
rollback() {
  ln -sfn "$previous" "$base/.current-pack-rollback"
  mv -Tf "$base/.current-pack-rollback" "$base/current"
  systemctl restart vlt-account-api vlt-account-admin
}
trap rollback ERR
ln -sfn "$release" "$base/.current-pack-next"
mv -Tf "$base/.current-pack-next" "$base/current"
systemctl restart vlt-account-api vlt-account-admin
for attempt in {1..20}; do
  if curl -fsS http://127.0.0.1:8080/readyz >/dev/null && curl -fsS http://127.0.0.1:8080/v1/plugin-packs >/dev/null; then break; fi
  sleep 1
done
curl -fsS http://127.0.0.1:8080/readyz
curl -fsS http://127.0.0.1:8080/v1/plugin-packs
curl -fsS -o /dev/null https://admin.vltstudio.ru/login
systemctl is-active vlt-account-api vlt-account-admin
trap - ERR
printf '\nDEPLOYED %s\n' "$release"
