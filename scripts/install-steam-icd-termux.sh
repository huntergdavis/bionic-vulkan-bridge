#!/data/data/com.termux/files/usr/bin/bash

set -euo pipefail
umask 077

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
steam_base=${STEAM_ARM64_BASE:-$HOME/steam-arm64}
source_dir=${BVB_GLIBC_OUTPUT_DIR:-$project_dir/out/triangle-dispatch-glibc}
source_library=$source_dir/libvulkan-bvb-glibc.so
source_service=${BVB_SERVICE_BINARY:-$project_dir/build/bvb-bridge-service}
install_root=$steam_base/bvb
library_dir=$install_root/lib
binary_dir=$install_root/bin
manifest_dir=$install_root/icd.d
library=$library_dir/libvulkan-bvb-glibc.so
service=$binary_dir/bvb-bridge-service
manifest=$manifest_dir/bvb_icd.aarch64.json
stamp=$install_root/install.sha256
backup_root=$install_root/backups
transaction_active=0
transaction_dir=
had_previous=0
expected_library_sha=
expected_service_sha=
expected_manifest_sha=
expected_stamp_sha=
old_library_sha=
old_service_sha=
old_manifest_sha=
old_stamp_sha=

fail() {
    printf 'install-steam-icd-termux: %s\n' "$*" >&2
    exit 1
}

durable_sync() {
    sync -f "$1" || return 1
}

write_transaction_state() {
    local state=$1 state_file=$transaction_dir/state
    local state_tmp=$transaction_dir/state.tmp.$BASHPID

    [[ ! -e $state_tmp && ! -L $state_tmp ]] || {
        printf 'install-steam-icd-termux: transaction state temporary path exists: %s\n' \
            "$state_tmp" >&2
        return 1
    }
    printf '%s\n' "$state" >"$state_tmp" || return 1
    durable_sync "$state_tmp" || return 1
    mv -f -- "$state_tmp" "$state_file" || return 1
    durable_sync "$transaction_dir" || return 1
}

record_has_exact_line() {
    local record=$1 expected=$2 count

    count=$(grep -Fxc -- "$expected" "$record" || true)
    [[ $count == 1 ]]
}

record_hash() {
    local record=$1 key=$2 line value

    line=$(grep -E "^${key}=[0-9a-f]{64}$" "$record" || true)
    [[ $line != *$'\n'* && -n $line ]] || return 1
    value=${line#*=}
    [[ $value =~ ^[0-9a-f]{64}$ ]] || return 1
    printf '%s\n' "$value"
}

load_transaction_record() {
    local record=$1

    record_has_exact_line "$record" "library=$library" || return 1
    record_has_exact_line "$record" "service=$service" || return 1
    record_has_exact_line "$record" "manifest=$manifest" || return 1
    record_has_exact_line "$record" "stamp=$stamp" || return 1
    expected_library_sha=$(record_hash "$record" library_sha256) || return 1
    expected_service_sha=$(record_hash "$record" service_sha256) || return 1
    expected_manifest_sha=$(record_hash "$record" manifest_sha256) || return 1
    expected_stamp_sha=$(record_hash "$record" stamp_sha256) || return 1
    if ((had_previous)); then
        old_library_sha=$(record_hash "$record" old_library_sha256) || return 1
        old_service_sha=$(record_hash "$record" old_service_sha256) || return 1
        old_manifest_sha=$(record_hash "$record" old_manifest_sha256) || return 1
        old_stamp_sha=$(record_hash "$record" old_stamp_sha256) || return 1
    else
        record_has_exact_line "$record" 'old_library_sha256=none' || return 1
        record_has_exact_line "$record" 'old_service_sha256=none' || return 1
        record_has_exact_line "$record" 'old_manifest_sha256=none' || return 1
        record_has_exact_line "$record" 'old_stamp_sha256=none' || return 1
        old_library_sha=none
        old_service_sha=none
        old_manifest_sha=none
        old_stamp_sha=none
    fi
}

validate_transaction_device() {
    local transaction_device directory

    transaction_device=$(stat -c %d "$transaction_dir") || return 1
    for directory in "$library_dir" "$binary_dir" "$manifest_dir" "$install_root"; do
        [[ $(stat -c %d "$directory") == "$transaction_device" ]] || {
            printf 'install-steam-icd-termux: transaction and target directory are on different devices: %s\n' \
                "$directory" >&2
            return 1
        }
    done
}

rollback_one() {
    local target=$1 backup=$2 staged=$3 failed=$4 mode=$5 label=$6 expected=$7
    local old_expected=$8
    local rollback_tmp target_sha backup_sha failed_sha staged_sha

    # A staged file disappears only after its atomic rename reached the target.
    if [[ -e $staged || -L $staged ]]; then
        [[ -f $staged && ! -L $staged ]] || {
            printf 'install-steam-icd-termux: staged rollback artifact is unsafe: %s\n' \
                "$staged" >&2
            return 1
        }
        staged_sha=$(sha256sum "$staged" | awk '{print $1}') || return 1
        [[ $staged_sha == "$expected" ]] || {
            printf 'install-steam-icd-termux: staged rollback artifact identity changed: %s\n' \
                "$staged" >&2
            return 1
        }
        if ((had_previous)); then
            [[ -f $target && ! -L $target && -f $backup && ! -L $backup ]] || {
                printf 'install-steam-icd-termux: staged upgrade rollback state is invalid: %s\n' \
                    "$label" >&2
                return 1
            }
            target_sha=$(sha256sum "$target" | awk '{print $1}') || return 1
            backup_sha=$(sha256sum "$backup" | awk '{print $1}') || return 1
            [[ $target_sha == "$old_expected" && $backup_sha == "$old_expected" ]] || {
                printf 'install-steam-icd-termux: staged upgrade rollback identity changed: %s\n' \
                    "$label" >&2
                return 1
            }
        else
            [[ ! -e $target && ! -L $target && ! -e $failed && ! -L $failed ]] || {
                printf 'install-steam-icd-termux: staged first-install rollback state is invalid: %s\n' \
                    "$label" >&2
                return 1
            }
        fi
        return 0
    fi
    if ((had_previous)); then
        [[ -e $target && -f $target && ! -L $target ]] || {
            printf 'install-steam-icd-termux: rollback target is unsafe: %s\n' \
                "$target" >&2
            return 1
        }
        [[ -f $backup && ! -L $backup ]] || {
            printf 'install-steam-icd-termux: rollback backup is unsafe: %s\n' \
                "$backup" >&2
            return 1
        }
        target_sha=$(sha256sum "$target" | awk '{print $1}') || return 1
        backup_sha=$(sha256sum "$backup" | awk '{print $1}') || return 1
        [[ $backup_sha == "$old_expected" ]] || {
            printf 'install-steam-icd-termux: rollback backup identity changed: %s\n' \
                "$backup" >&2
            return 1
        }
        [[ $target_sha != "$old_expected" ]] || return 0
        [[ $target_sha == "$expected" ]] || {
            printf 'install-steam-icd-termux: rollback target identity changed: %s\n' \
                "$target" >&2
            return 1
        }
        rollback_tmp=$target.rollback.$BASHPID
        [[ ! -e $rollback_tmp && ! -L $rollback_tmp ]] || {
            printf 'install-steam-icd-termux: rollback temporary path exists: %s\n' \
                "$rollback_tmp" >&2
            return 1
        }
        install -m "$mode" "$backup" "$rollback_tmp" || return 1
        durable_sync "$rollback_tmp" || return 1
        mv -f -- "$rollback_tmp" "$target" || return 1
        target_sha=$(sha256sum "$target" | awk '{print $1}') || return 1
        [[ $target_sha == "$old_expected" ]] || {
            printf 'install-steam-icd-termux: restored rollback target hash is invalid: %s\n' \
                "$target" >&2
            return 1
        }
    elif [[ -e $target || -L $target ]]; then
        [[ -f $target && ! -L $target && ! -e $failed && ! -L $failed ]] || {
            printf 'install-steam-icd-termux: failed-artifact path exists: %s\n' \
                "$failed" >&2
            return 1
        }
        target_sha=$(sha256sum "$target" | awk '{print $1}') || return 1
        [[ $target_sha == "$expected" ]] || {
            printf 'install-steam-icd-termux: rollback target identity changed: %s\n' \
                "$target" >&2
            return 1
        }
        mv -- "$target" "$failed" || return 1
    else
        [[ -f $failed && ! -L $failed ]] || {
            printf 'install-steam-icd-termux: first-install rollback state is invalid: %s\n' \
                "$label" >&2
            return 1
        }
        failed_sha=$(sha256sum "$failed" | awk '{print $1}') || return 1
        [[ $failed_sha == "$expected" ]] || {
            printf 'install-steam-icd-termux: failed-artifact identity changed: %s\n' \
                "$failed" >&2
            return 1
        }
    fi
    printf 'install-steam-icd-termux: rolled back %s\n' "$label" >&2
}

rollback_install() {
    local rollback_status=0

    set +e
    rollback_one "$stamp" "$transaction_dir/old-install.sha256" \
        "$transaction_dir/new-install.sha256" \
        "$transaction_dir/failed-install.sha256" 600 stamp \
        "$expected_stamp_sha" "$old_stamp_sha" || rollback_status=1
    rollback_one "$manifest" "$transaction_dir/old-bvb_icd.aarch64.json" \
        "$transaction_dir/new-bvb_icd.aarch64.json" \
        "$transaction_dir/failed-bvb_icd.aarch64.json" 600 manifest \
        "$expected_manifest_sha" "$old_manifest_sha" || \
        rollback_status=1
    rollback_one "$service" "$transaction_dir/old-bvb-bridge-service" \
        "$transaction_dir/new-bvb-bridge-service" \
        "$transaction_dir/failed-bvb-bridge-service" 700 service \
        "$expected_service_sha" "$old_service_sha" || \
        rollback_status=1
    rollback_one "$library" "$transaction_dir/old-libvulkan-bvb-glibc.so" \
        "$transaction_dir/new-libvulkan-bvb-glibc.so" \
        "$transaction_dir/failed-libvulkan-bvb-glibc.so" 700 library \
        "$expected_library_sha" "$old_library_sha" || \
        rollback_status=1
    set -e
    if ((rollback_status)); then
        printf 'install-steam-icd-termux: ROLLBACK INCOMPLETE; preserve %s\n' \
            "$transaction_dir" >&2
        return 1
    fi
    durable_sync "$install_root" || return 1
    write_transaction_state rolled-back || return 1
    printf 'install-steam-icd-termux: rollback complete; evidence=%s\n' \
        "$transaction_dir" >&2
}

on_exit() {
    local status=$?

    trap - EXIT INT TERM HUP
    if ((transaction_active)); then
        rollback_install || status=1
    fi
    exit "$status"
}

trap 'exit 130' INT TERM HUP
trap on_exit EXIT

[[ $steam_base == /* && -d $steam_base && ! -L $steam_base ]] ||
    fail "Steam base is unavailable or unsafe: $steam_base"
for directory in \
    "$install_root" "$library_dir" "$binary_dir" "$manifest_dir" "$backup_root"; do
    if [[ -e $directory || -L $directory ]]; then
        [[ -d $directory && ! -L $directory ]] ||
            fail "install directory is unavailable or unsafe: $directory"
    else
        mkdir -m 700 -- "$directory"
    fi
done
lock_file=$backup_root/install.lock
[[ ! -L $lock_file && (! -e $lock_file || -f $lock_file) ]] ||
    fail "install lock is unavailable or unsafe: $lock_file"
command -v flock >/dev/null 2>&1 || fail 'flock is required for installer safety'
command -v sync >/dev/null 2>&1 || fail 'sync is required for installer durability'
exec 9>"$lock_file"
flock -n 9 || fail 'another BVB installer is already running'

prepared_transactions=()
shopt -s nullglob
for candidate in "$backup_root"/install-pre-*; do
    [[ -d $candidate && ! -L $candidate ]] ||
        fail "transaction path is unavailable or unsafe: $candidate"
    state_file=$candidate/state
    [[ -e $state_file || -L $state_file ]] || continue
    [[ -f $state_file && ! -L $state_file ]] ||
        fail "transaction state is unavailable or unsafe: $state_file"
    state=$(<"$state_file")
    case $state in
        prepared) prepared_transactions+=("$candidate") ;;
        committed | rolled-back) ;;
        *) fail "transaction has an unknown state: $candidate" ;;
    esac
done
shopt -u nullglob
((${#prepared_transactions[@]} <= 1)) ||
    fail 'multiple interrupted transactions require manual recovery'
if ((${#prepared_transactions[@]} == 1)); then
    transaction_dir=${prepared_transactions[0]}
    transaction_record=$transaction_dir/transaction.txt
    [[ -f $transaction_record && ! -L $transaction_record ]] ||
        fail "interrupted transaction record is unsafe: $transaction_record"
    if record_has_exact_line "$transaction_record" 'had_previous=1' &&
        ! grep -Fxq 'had_previous=0' "$transaction_record"; then
        had_previous=1
    elif record_has_exact_line "$transaction_record" 'had_previous=0' &&
        ! grep -Fxq 'had_previous=1' "$transaction_record"; then
        had_previous=0
    else
        fail "interrupted transaction has invalid prior-state metadata: $transaction_dir"
    fi
    load_transaction_record "$transaction_record" ||
        fail "interrupted transaction record does not match installed targets: $transaction_dir"
    validate_transaction_device ||
        fail "interrupted transaction is not on the installed target device: $transaction_dir"
    transaction_active=1
    printf 'install-steam-icd-termux: recovering interrupted transaction: %s\n' \
        "$transaction_dir" >&2
    rollback_install || fail 'interrupted transaction recovery failed'
    transaction_active=0
    transaction_dir=
    had_previous=0
fi

[[ -f $source_library && ! -L $source_library ]] ||
    fail "glibc ICD is unavailable or unsafe: $source_library"
[[ -x $source_service && ! -L $source_service ]] ||
    fail "Bionic service is unavailable or unsafe: $source_service"
readelf -h "$source_library" | grep -Fq 'AArch64' ||
    fail 'glibc ICD is not AArch64'
readelf -l "$source_library" | grep -Fq '/lib/ld-linux-aarch64.so.1' &&
    fail 'glibc ICD unexpectedly has a program interpreter'
readelf -l "$source_service" | grep -Fq '/system/bin/linker64' ||
    fail 'bridge service is not Android Bionic'

current_count=0
for target in "$library" "$service" "$manifest" "$stamp"; do
    if [[ -e $target || -L $target ]]; then
        [[ -f $target && ! -L $target ]] ||
            fail "installed target is unavailable or unsafe: $target"
        current_count=$((current_count + 1))
    fi
done
((current_count == 0 || current_count == 4)) ||
    fail 'refusing a partial prior BVB installation; restore or remove it first'
((current_count == 0)) || had_previous=1

transaction_dir=$(mktemp -d "$backup_root/install-pre-XXXXXXXX")
[[ $transaction_dir == "$backup_root"/install-pre-* &&
    -d $transaction_dir && ! -L $transaction_dir ]] ||
    fail 'could not create a safe install transaction directory'
chmod 700 "$transaction_dir"
validate_transaction_device ||
    fail "transaction is not on the installed target device: $transaction_dir"
stage_library=$transaction_dir/new-libvulkan-bvb-glibc.so
stage_service=$transaction_dir/new-bvb-bridge-service
stage_manifest=$transaction_dir/new-bvb_icd.aarch64.json
stage_stamp=$transaction_dir/new-install.sha256

install -m 700 "$source_library" "$stage_library"
install -m 700 "$source_service" "$stage_service"
printf '%s\n' \
    '{' \
    '  "file_format_version": "1.0.1",' \
    '  "ICD": {' \
    "    \"library_path\": \"$library\"," \
    '    "api_version": "1.3.0",' \
    '    "library_arch": "64"' \
    '  }' \
    '}' >"$stage_manifest"
chmod 600 "$stage_manifest"

library_sha=$(sha256sum "$stage_library" | awk '{print $1}')
service_sha=$(sha256sum "$stage_service" | awk '{print $1}')
manifest_sha=$(sha256sum "$stage_manifest" | awk '{print $1}')
printf '%s  %s\n%s  %s\n%s  %s\n' \
    "$library_sha" "$library" \
    "$service_sha" "$service" \
    "$manifest_sha" "$manifest" >"$stage_stamp"
chmod 600 "$stage_stamp"
stamp_sha=$(sha256sum "$stage_stamp" | awk '{print $1}')
expected_library_sha=$library_sha
expected_service_sha=$service_sha
expected_manifest_sha=$manifest_sha
expected_stamp_sha=$stamp_sha

if ((had_previous)); then
    cp -p -- "$library" "$transaction_dir/old-libvulkan-bvb-glibc.so"
    cp -p -- "$service" "$transaction_dir/old-bvb-bridge-service"
    cp -p -- "$manifest" "$transaction_dir/old-bvb_icd.aarch64.json"
    cp -p -- "$stamp" "$transaction_dir/old-install.sha256"
    old_library_sha=$(sha256sum "$transaction_dir/old-libvulkan-bvb-glibc.so" | awk '{print $1}')
    old_service_sha=$(sha256sum "$transaction_dir/old-bvb-bridge-service" | awk '{print $1}')
    old_manifest_sha=$(sha256sum "$transaction_dir/old-bvb_icd.aarch64.json" | awk '{print $1}')
    old_stamp_sha=$(sha256sum "$transaction_dir/old-install.sha256" | awk '{print $1}')
    [[ $(sha256sum "$library" | awk '{print $1}') == "$old_library_sha" &&
        $(sha256sum "$service" | awk '{print $1}') == "$old_service_sha" &&
        $(sha256sum "$manifest" | awk '{print $1}') == "$old_manifest_sha" &&
        $(sha256sum "$stamp" | awk '{print $1}') == "$old_stamp_sha" ]] ||
        fail 'installed targets changed while their rollback snapshot was created'
else
    old_library_sha=none
    old_service_sha=none
    old_manifest_sha=none
    old_stamp_sha=none
fi
printf '%s\n' \
    "had_previous=$had_previous" \
    "library=$library" \
    "library_sha256=$library_sha" \
    "service=$service" \
    "service_sha256=$service_sha" \
    "manifest=$manifest" \
    "manifest_sha256=$manifest_sha" \
    "stamp=$stamp" \
    "stamp_sha256=$stamp_sha" \
    "old_library_sha256=$old_library_sha" \
    "old_service_sha256=$old_service_sha" \
    "old_manifest_sha256=$old_manifest_sha" \
    "old_stamp_sha256=$old_stamp_sha" \
    >"$transaction_dir/transaction.txt"
durable_sync "$transaction_dir"
write_transaction_state prepared

transaction_active=1
mv -f -- "$stage_library" "$library"
mv -f -- "$stage_service" "$service"
mv -f -- "$stage_manifest" "$manifest"
mv -f -- "$stage_stamp" "$stamp"

[[ $(sha256sum "$library" | awk '{print $1}') == "$library_sha" ]] ||
    fail 'installed glibc ICD hash does not match the staged artifact'
[[ $(sha256sum "$service" | awk '{print $1}') == "$service_sha" ]] ||
    fail 'installed service hash does not match the staged artifact'
[[ $(sha256sum "$manifest" | awk '{print $1}') == "$manifest_sha" ]] ||
    fail 'installed manifest hash does not match the staged artifact'
sha256sum -c "$stamp" >/dev/null || fail 'installed checksum stamp is invalid'
durable_sync "$install_root"
write_transaction_state committed
transaction_active=0

printf 'BVB Steam ICD installed: manifest=%s service=%s backup=%s\n' \
    "$manifest" "$service" "$transaction_dir"
