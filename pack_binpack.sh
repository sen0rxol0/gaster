#!/usr/bin/env bash
set -euo pipefail

# sudo LDID=/Applications/ra1ntool.app/Contents/MacOS/bin/gastera1n/ldid2 ./pack_binpack.sh
# split -b 2500000 ssh64.tar.gz ssh64.tar.gz_
# tar -tzf ssh64.tar.gz | sort > ssh64tar.txt

# tar -C ssh64/ --preserve-permissions -xf ssh64.tar.gz

# ---------------------------------------------------------------------------
# build_binpack.sh — assemble ssh64.tar.gz for gastera1n SSH ramdisk
#
# Downloads Bingner .deb packages, extracts them into a staging tree,
# signs binaries with appropriate per-binary entitlements, and produces
# ssh64.tar.gz in the current directory.
# ---------------------------------------------------------------------------

LDID="${LDID:-ldid2}"
STAGING=".binpack/staging"
DEBS=".binpack/debs"
DEB_TMP=".binpack/deb_tmp"
OUTPUT="ssh64.tar.gz"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

die()  { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }
log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mWARN:\033[0m %s\n' "$*" >&2; }

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1"
}

require_cmd curl
require_cmd ar
require_cmd tar
require_cmd "${LDID}"

# ---------------------------------------------------------------------------
# Download
# ---------------------------------------------------------------------------
mkdir -p "${DEBS}"

download() {
    local url="$1"
    local out="${DEBS}/$(basename "${url}")"
    if [[ -f "${out}" ]]; then
        log "  cached: $(basename "${url}")"
        return 0
    fi
    log "  downloading: $(basename "${url}")"
    curl -fL --retry 3 --retry-delay 2 -o "${out}" "${url}" \
        || die "Failed to download: ${url}"
}

# iOS 9+
BASE_BINGNER="https://apt.bingner.com/debs/1443.00"
# iOS 14+
BASE_PROCURSUS="https://apt.procurs.us/pool/main/iphoneos-arm64/1700"

# https://apt.procurs.us/pool/main/iphoneos-arm64/1700/libtommath1_1.2.0_iphoneos-arm.deb
# https://apt.procurs.us/pool/main/iphoneos-arm64/1700/libtomcrypt1_1.18.2_iphoneos-arm.deb
# https://apt.procurs.us/pool/main/iphoneos-arm64/1700/openpam/libpam2_20230627_iphoneos-arm.deb

coreutils_deb="coreutils_9.5_iphoneos-arm.deb"
findutils_deb="findutils_4.6.0-2_iphoneos-arm.deb"
shell_cmds_deb="shell-cmds_118-8_iphoneos-arm.deb"
ncurses5_libs_deb="ncurses5-libs_5.9-1_iphoneos-arm.deb"
plutil_deb="com.bingner.plutil_0.2.1_iphoneos-arm.deb"
tar_deb="tar_1.33-1_iphoneos-arm.deb"
launchctl_deb="launchctl-25_iphoneos-arm.deb"
readline_deb="readline_8.0-1_iphoneos-arm.deb"
sed_deb="sed_4.5-1_iphoneos-arm.deb"
grep_deb="grep_3.1-1_iphoneos-arm.deb"
bash_deb="bash_5.0.3-2_iphoneos-arm.deb"
# Core filesystem tools — mv, cp, chmod, chown, mkdir, rm, cat, dd, chflags
download "${BASE_PROCURSUS}/coreutils/$coreutils_deb"
download "${BASE_BINGNER}/$findutils_deb"
# reboot, chflags
download "${BASE_BINGNER}/$shell_cmds_deb"
download "${BASE_BINGNER}/$ncurses5_libs_deb"
download "${BASE_BINGNER}/$readline_deb"
download "${BASE_BINGNER}/$sed_deb"
download "${BASE_BINGNER}/$grep_deb"
download "${BASE_BINGNER}/$bash_deb"
# openssh — sshd and scp, the actual transport layer
# download "${BASE_BINGNER}/libssl1.1.1_1.1.1n-1_iphoneos-arm.deb"
# download "${BASE_BINGNER}/openssh-server_8.4-3_iphoneos-arm.deb"
# download "${BASE_BINGNER}/openssh-client_8.4-3_iphoneos-arm.deb"

download "${BASE_BINGNER}/$plutil_deb"
download "${BASE_BINGNER}/$tar_deb"
download "${BASE_BINGNER}/$launchctl_deb"

# ---------------------------------------------------------------------------
# Extract
# ---------------------------------------------------------------------------

# Extract a .deb's data archive into the staging tree.
# Handles data.tar.gz / data.tar.xz / data.tar.zst transparently.
extract_deb() {
    local deb="$1"
    log "Extracting $(basename "${deb}")"

    rm -rf "${DEB_TMP}"
    mkdir -p "${DEB_TMP}"

    # ar on macOS does not support --output; use a subshell cd instead.
    (cd "${DEB_TMP}" && ar x "${OLDPWD}/${deb}") \
        || die "ar failed on $(basename "${deb}")"

    local data_archive
    data_archive="$(find "${DEB_TMP}" -maxdepth 1 -name 'data.*' | head -1)"
    [[ -n "${data_archive}" ]] \
        || die "No data archive found in $(basename "${deb}")"

    case "${data_archive}" in
        *.tar.zst)
            zstd -d --stdout "${data_archive}" | tar -xf - -C "$STAGING"
            ;;
        *)
            tar -xf "${data_archive}" -k -C "${STAGING}"
            ;;
    esac

    rm -rf "${DEB_TMP}"
}

rm -rf "${STAGING}"
mkdir -p "${STAGING}"

extract_deb "${DEBS}/$coreutils_deb"
extract_deb "${DEBS}/$findutils_deb"
extract_deb "${DEBS}/$shell_cmds_deb"
extract_deb "${DEBS}/$ncurses5_libs_deb"
extract_deb "${DEBS}/$readline_deb"
extract_deb "${DEBS}/$sed_deb"
extract_deb "${DEBS}/$grep_deb"
extract_deb "${DEBS}/$bash_deb"
# extract_deb "${DEBS}/libssl1.1.1_1.1.1n-1_iphoneos-arm.deb"
# extract_deb "${DEBS}/openssh-server_8.4-3_iphoneos-arm.deb"
# extract_deb "${DEBS}/openssh-client_8.4-3_iphoneos-arm.deb"

extract_deb "${DEBS}/$tar_deb"
extract_deb "${DEBS}/$launchctl_deb"
extract_deb "${DEBS}/$plutil_deb"

log "Pruning unnecessary files"

# # Remove everything OpenSSH installed
# rm -rf "${STAGING}/etc/ssh"
# rm -f  "${STAGING}/usr/sbin/sshd"
# rm -f  "${STAGING}/usr/libexec/ssh-"*
# rm -f  "${STAGING}/usr/libexec/sshd-keygen-wrapper"
# rm -f  "${STAGING}/usr/bin/ssh-"*
# # # OpenSSH binaries — replaced by dropbear
# # rm -f  "${STAGING}/usr/bin/ssh"
# rm -f  "${STAGING}/usr/bin/scp"
# # rm -f  "${STAGING}/usr/bin/sftp"
# # rm -f  "${STAGING}/usr/libexec/sftp-server"
# # # OpenSSL libs — dropbear has its own crypto, these came from openssh debs
# # rm -f  "${STAGING}/usr/lib/libssl.1.1.dylib"
# # rm -f  "${STAGING}/usr/lib/libssl.a"
# # rm -f  "${STAGING}/usr/lib/libcrypto.1.1.dylib"
# # rm -f  "${STAGING}/usr/lib/libcrypto.a"
# # rm -rf "${STAGING}/usr/lib/engines-1.1"

# Build-time only artifacts
rm -rf "${STAGING}/usr/include"
rm -rf "${STAGING}/usr/lib/pkgconfig"
rm -rf "${STAGING}/usr/lib/bash"
rm -f  "${STAGING}/usr/bin/bashbug"

# findutils locate/updatedb — not used by ssh_ramdisk.sh
rm -f  "${STAGING}/usr/bin/locate"
rm -f  "${STAGING}/usr/bin/updatedb"
rm -f  "${STAGING}/usr/libexec/bigram"
rm -f  "${STAGING}/usr/libexec/code"
rm -f  "${STAGING}/usr/libexec/frcode"

# Documentation and locale data — not needed on ramdisk
rm -rf "${STAGING}/usr/share/doc"
rm -rf "${STAGING}/usr/share/locale"
rm -rf "${STAGING}/usr/share/man"

# coreutils binaries not needed for ssh_ramdisk.sh operations
COREUTILS_REMOVE=(
    # checksums / hashing
    b2sum cksum md5sum sha1sum sha224sum sha256sum sha384sum sha512sum sum
    # text processing
    base32 base64 basenc comm csplit cut expand expr factor fmt fold
    head nl numfmt od paste pr ptx seq shred shuf sort split tac tail
    tee tr truncate tsort unexpand uniq wc yes
    # system / process info
    chcon chroot dircolors du groups hostid id logname nice nohup nproc
    pinky printenv renice runcon stat stdbuf timeout uptime users who
    whoami
    # misc
    dirname install join link mkfifo mknod pathchk script
    vdir
)

log "Removing unused coreutils binaries"
for bin in "${COREUTILS_REMOVE[@]}"; do
    rm -f "${STAGING}/usr/bin/${bin}"
    rm -f "${STAGING}/bin/${bin}"
done

rm -rf "${STAGING}/usr/libexec/coreutils"
# coreutils profile.d shim — sets PATH aliases, not needed
rm -f "${STAGING}/etc/profile.d/coreutils.sh"
# log "Stripping quarantine xattr from staging tree"
# xattr -r -d com.apple.quarantine "${STAGING}" 2>/dev/null || true

# ---------------------------------------------------------------------------
# Inject required dylibs
#
# libiconv and libresolv are present on the ramdisk OS volume but may not
# be in the ramdisk itself. Dropbear and bash both depend on them.
# libncurses.5 is shipped by the ncurses deb; libncurses.5.4 is the actual
# versioned dylib that some binaries link against — add a symlink so both
# names resolve.
# ---------------------------------------------------------------------------
log "Injecting required dylibs"
mkdir -p "${STAGING}/usr/lib"

for lib in libiconv.2 libresolv.9; do
    src="${SCRIPT_DIR}/lib/${lib}.dylib"
    [[ -f "${src}" ]] \
        || die "Required dylib not found: lib/${lib}"
    install -m 755 "${src}" "${STAGING}/usr/lib/${lib}.dylib"
    log "  injected: ${lib}"
done

ln -sf libiconv.2.dylib "${STAGING}/usr/lib/libiconv.dylib"

# libncurses.5.4 → libncurses.5.dylib symlink
# The versioned name is what some binaries (bash, readline-linked tools)
# encode in their LC_LOAD_DYLIB; the deb ships the .5 name only.
if [[ -f "${STAGING}/usr/lib/libncurses.5.dylib" ]]; then
    ln -sf libncurses.5.dylib "${STAGING}/usr/lib/libncurses.5.4.dylib"
    log "  symlinked: libncurses.5.4.dylib → libncurses.5.dylib"
else
    warn "libncurses.5.dylib not found in staging — ncurses deb may not have extracted correctly"
fi

# Dropbear generate key
# run palera1n or checkra1n?
# iproxy 2222 44 &
# ssh root@localhost -p2222
# alpine
# dropbearkey -t rsa -s 2048 -f /private/var/mobile/Downloads/dropbear_rsa_host_key
# dropbearkey -t ed25519 -f /private/var/mobile/Downloads/dropbear_ed25519_host_key
# dropbearkey -t ecdsa -f /private/var/mobile/Downloads/dropbear_ecdsa_host_key
# dropbearkey -t dss -f /private/var/mobile/Downloads/dropbear_dss_host_key
# exit
# scp -P2222 root@localhost:/private/var/mobile/Downloads/dropbear_rsa_host_key ./dropbear_rsa_host_key
# scp -P2222 root@localhost:/private/var/mobile/Downloads/dropbear_ed25519_host_key ./dropbear_ed25519_host_key
# scp -P2222 root@localhost:/private/var/mobile/Downloads/dropbear_ecdsa_host_key ./dropbear_ecdsa_host_key
# scp -P2222 root@localhost:/private/var/mobile/Downloads/dropbear_dss_host_key ./dropbear_dss_host_key
# killall iproxy

# ---------------------------------------------------------------------------
# Inject Dropbear host key and binaries
#
# dropbear, dbclient, and scp are built from source by build_dropbear_ios.sh
# and expected alongside dropbear_rsa_host_key,dropbear_ed25519_host_key in the dropbear/ subdirectory.
# restored_external invokes dropbear at /usr/local/bin/dropbear.
# ---------------------------------------------------------------------------
log "Injecting Dropbear host key"
mkdir -p "${STAGING}/etc/dropbear"

for host_key in dropbear_rsa_host_key dropbear_ed25519_host_key dropbear_ecdsa_host_key dropbear_dss_host_key; do
  [[ -f "${SCRIPT_DIR}/dropbear/${host_key}" ]] \
      || die "Required file not found: dropbear/${host_key}"
  install -m 600 "${SCRIPT_DIR}/dropbear/${host_key}" \
      "${STAGING}/etc/dropbear/${host_key}"
done

log "Injecting Dropbear binaries"
mkdir -p "${STAGING}/usr/local/bin"
mkdir -p "${STAGING}/usr/bin"

for bin in dbclient dropbear scp; do
    src="${SCRIPT_DIR}/dropbear/${bin}"
    [[ -f "${src}" ]] \
        || die "Required binary not found: dropbear/${bin} (run build_dropbear_ios.sh first)"
    case "${bin}" in
        scp)
            install -m 755 "${src}" "${STAGING}/usr/bin/scp"
            log "  injected: usr/bin/scp"
            ;;
        *)
            install -m 755 "${src}" "${STAGING}/usr/local/bin/${bin}"
            log "  injected: usr/local/bin/${bin}"
            ;;
    esac
done
# ---------------------------------------------------------------------------
# Inject custom binaries
#
# mountfs and restored_external are project-built binaries not available
# from any deb repository. They are expected in the working directory
# alongside this script.
# ---------------------------------------------------------------------------

log "Injecting custom binaries"
mkdir -p "${STAGING}/usr/local/bin"

mountfs_src="${SCRIPT_DIR}/mountfs"
restored_external_src="${SCRIPT_DIR}/restored_external"

[[ -f "${mountfs_src}" ]] \
    || die "Required binary not found in working directory: mountfs"
install -m 755 "${mountfs_src}" "${STAGING}/usr/local/bin/mountfs"
log "  injected: mountfs"

[[ -f "${restored_external_src}" ]] \
    || die "Required binary not found in working directory: restored_external"
install -m 755 "${restored_external_src}" "${STAGING}/usr/local/bin/restored_external"
log "  injected: restored_external"

# ---------------------------------------------------------------------------
# Entitlement sets
#
# Three tiers:
#
#   ENTS_PRIV  — full ramdisk privileges; for binaries that need to operate
#                on the live filesystem, manage processes, or load arbitrary
#                code (launchctl, mount helpers, restored_external).
#
#   ENTS_BASIC — skip-library-validation only; for binaries that link
#                against unsigned or ad-hoc-signed dylibs in the ramdisk
#                but do not need platform-application privileges (most
#                userland tools: bash, grep, sed, coreutils, etc.).
#
#   (platform-application)  — binary is signed ad-hoc with platform-application set to true;
# ---------------------------------------------------------------------------

# restored_external
write_ents_restored_external() {
    cat > "$1" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>platform-application</key>
    <true/>
    <key>com.apple.afu.userclientaccess</key>
    <true/>
    <key>com.apple.aop.rose.controller.admin</key>
    <true/>
    <key>com.apple.camera.iokit-user-access</key>
    <true/>
    <key>com.apple.cyrus.allow</key>
    <true/>
    <key>com.apple.driver.AppleBasebandPCI.user-access</key>
    <true/>
    <key>com.apple.driver.AppleBasebandPCIControl.user-access</key>
    <true/>
    <key>com.apple.gasgauge.user-access-device</key>
    <true/>
    <key>com.apple.hid.system.user-access-service</key>
    <true/>
    <key>com.apple.keystore.absinthe</key>
    <true/>
    <key>com.apple.keystore.fdr-access</key>
    <true/>
    <key>com.apple.keystore.obliterate-d-key</key>
    <true/>
    <key>com.apple.keystore.sik.access</key>
    <true/>
    <key>com.apple.libFDR.AllowIdentifierOverride</key>
    <true/>
    <key>com.apple.mobileactivationd.spi</key>
    <true/>
    <key>com.apple.private.IOAESAccelerator.fdr-key-handle</key>
    <true/>
    <key>com.apple.private.MobileGestalt.AllowedProtectedKeys</key>
    <true/>
    <key>com.apple.private.ProvInfoIOKitUserClient.access</key>
    <true/>
    <key>com.apple.private.PurpleReverseProxy.allowed</key>
    <true/>
    <key>com.apple.private.apfs.create-sealed-snapshot</key>
    <true/>
    <key>com.apple.private.apfs.set-firmlink</key>
    <true/>
    <key>com.apple.private.apfs.trim-active-file</key>
    <true/>
    <key>com.apple.private.apfs.xart</key>
    <true/>
    <key>com.apple.private.applemesa.allow</key>
    <true/>
    <key>com.apple.private.applepearl.allow</key>
    <true/>
    <key>com.apple.private.applesepmanager.allow</key>
    <true/>
    <key>com.apple.private.applesmc.user-access</key>
    <true/>
    <key>com.apple.private.applesse.allow</key>
    <true/>
    <key>com.apple.private.gasgauge-update</key>
    <true/>
    <key>com.apple.private.hid.client.event-monitor</key>
    <true/>
    <key>com.apple.private.ioavfamily.user-access-displaymemory</key>
    <true/>
    <key>com.apple.private.iokit.system-nvram-allow</key>
    <true/>
    <key>com.apple.private.iowatchdog.user-access</key>
    <true/>
    <key>com.apple.private.security.bootpolicy</key>
    <true/>
    <key>com.apple.private.security.disk-device-access</key>
    <true/>
    <key>com.apple.private.security.nvram.recovery-boot-mode</key>
    <true/>
    <key>com.apple.private.security.restricted-nvram-variables.heritable</key>
    <true/>
    <key>com.apple.private.stockholm.allow</key>
    <true/>
    <key>com.apple.private.usbdevice.setdescription</key>
    <true/>
    <key>com.apple.private.vfs.snapshot</key>
    <true/>
    <key>com.apple.security.attestation.access</key>
    <true/>
    <key>com.apple.security.iokit-user-client-class</key>
    <array>
    	<string>IOWatchdogUserClient</string>
    	<string>AppleEffaceableStorageUserClient</string>
    	<string>AppleAPFSUserClient</string>
    	<string>IOAccessoryManagerUserClient </string>
    	<string>AppleImage3NORAccessUserClient</string>
    	<string>ASPUserClient</string>
    	<string>IOAESAcceleratorUserClient</string>
    </array>
    <key>com.apple.security.network.client</key>
    <true/>
    <key>com.apple.security.network.server</key>
    <true/>
    <key>com.apple.security.system-group-containers</key>
    <array>
    	<string>systemgroup.com.apple.mobileactivationd</string>
    </array>
</dict>
</plist>
EOF
}

# mountfs
write_ents_mountfs() {
    cat > "$1" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>platform-application</key>
    <true/>
    <key>com.apple.private.security.container-required</key>
    <false/>
    <key>com.apple.private.apfs.xart</key>
    <true/>
    <key>com.apple.private.security.disk-device-access</key>
    <true/>
    <key>com.apple.security.iokit-user-client-class</key>
    <array>
    	<string>AppleAPFSUserClient</string>
    </array>
</dict>
</plist>
EOF
}

write_ents_disk() {
    cat > "$1" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>platform-application</key>
    <true/>
    <key>com.apple.private.security.container-required</key>
  	<false/>
    <key>com.apple.private.skip-library-validation</key>
    <true/>
  	<key>com.apple.private.security.disk-device-access</key>
  	<true/>
</dict>
</plist>
EOF
}

write_ents_priv() {
    cat > "$1" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>platform-application</key>
    <true/>
    <key>com.apple.private.security.container-required</key>
  	<false/>
    <key>com.apple.private.skip-library-validation</key>
    <true/>
</dict>
</plist>
EOF
}

write_ents_basic() {
    cat > "$1" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>platform-application</key>
    <true/>
    <key>com.apple.private.security.container-required</key>
    <false/>
</dict>
</plist>
EOF
}

ENTS_MOUNTFS="${STAGING}/.ents_mountfs.xml"
ENTS_RESTORED="${STAGING}/.ents_restored_external.xml"
ENTS_DISK="${STAGING}/.ents_disk.xml"
ENTS_PRIV="${STAGING}/.ents_priv.xml"
ENTS_BASIC="${STAGING}/.ents_basic.xml"
write_ents_mountfs           "${ENTS_MOUNTFS}"
write_ents_restored_external "${ENTS_RESTORED}"
write_ents_disk              "${ENTS_DISK}"
write_ents_priv              "${ENTS_PRIV}"
write_ents_basic             "${ENTS_BASIC}"

# ---------------------------------------------------------------------------
# Signing
#
# is_macho()  — returns 0 if the file is an ARM Mach-O (skips fat x86,
#               dyld shared cache segments, and non-binaries).
# sign()      — signs a single Mach-O with the given entitlements file,
#               or ad-hoc if no entitlements file is supplied.
# sign_dir()  — iterates a directory and calls sign() on each Mach-O.
# ---------------------------------------------------------------------------


DISK_BINARIES=(
    "${STAGING}/usr/bin/dd"
    "${STAGING}/usr/bin/cat"
    "${STAGING}/usr/bin/plutil"
)

PRIV_BINARIES=(
    "${STAGING}/usr/bin/chmod"
    "${STAGING}/usr/bin/cp"
)

is_macho() {
    [[ -f "$1" ]] && file -b "$1" 2>/dev/null | grep -q 'Mach-O'
}

sign() {
    local bin="$1"
    local ents="${2:-}"
    is_macho "${bin}" || return 0
    log "  signing: ${bin##${STAGING}/}"
    if [[ -n "${ents}" ]]; then
        "${LDID}" -S"${ents}" "${bin}" \
            || warn "ldid2 failed on ${bin##${STAGING}/} — continuing"
    else
        "${LDID}" -S "${bin}" \
            || warn "ldid2 (ad-hoc) failed on ${bin##${STAGING}/} — continuing"
    fi
}

sign_dir() {
    local dir="${STAGING}/$1"
    local ents="${2:-}"
    [[ -d "${dir}" ]] || return 0
    local f
    for f in "${dir}"/*; do
        local already_signed=0
        local reserved_bin
        for reserved_bin in "${PRIV_BINARIES[@]}" "${DISK_BINARIES[@]}" \
                            "${STAGING}/usr/local/bin/mountfs" \
                            "${STAGING}/usr/local/bin/restored_external"; do
            if [[ "${f}" == "${reserved_bin}" ]]; then
                already_signed=1
                break
            fi
        done
        [[ "${already_signed}" -eq 1 ]] && continue
        sign "${f}" "${ents}"
    done
}

# Per-binary signing for custom injected tools
log "Signing mountfs"
sign "${STAGING}/usr/local/bin/mountfs"           "${ENTS_MOUNTFS}"

log "Signing restored_external"
sign "${STAGING}/usr/local/bin/restored_external" "${ENTS_RESTORED}"

log "Signing disk-access binaries"
for bin in "${DISK_BINARIES[@]}"; do
    sign "${bin}" "${ENTS_DISK}"
done

log "Signing privileged binaries"
for bin in "${PRIV_BINARIES[@]}"; do
    sign "${bin}" "${ENTS_PRIV}"
done

log "Signing userland binaries (basic entitlements)"
sign_dir "usr/local/bin" "${ENTS_BASIC}"
sign_dir "usr/sbin"     "${ENTS_BASIC}"
sign_dir "usr/bin"      "${ENTS_BASIC}"
sign_dir "bin"          "${ENTS_BASIC}"


log "Signing libexec (basic entitlements)"
sign_dir "usr/libexec"  "${ENTS_BASIC}"

# ---------------------------------------------------------------------------
# Permissions
# ---------------------------------------------------------------------------

log "Normalizing permissions"

# Directories
find "${STAGING}" -type d -exec chmod 0755 {} +

# Default all files to non-executable; executable dirs override below
find "${STAGING}" -type f -exec chmod 0644 {} +

# Executable binary directories
chmod -R 0755 "${STAGING}/bin"          2>/dev/null || true
chmod -R 0755 "${STAGING}/usr/bin"      2>/dev/null || true
chmod -R 0755 "${STAGING}/usr/libexec"  2>/dev/null || true
[[ -d "${STAGING}/usr/sbin"       ]] && chmod -R 0755 "${STAGING}/usr/sbin"
[[ -d "${STAGING}/usr/local/bin"  ]] && chmod -R 0755 "${STAGING}/usr/local/bin"

# Dylibs — read-only, not executable
find "${STAGING}/usr/lib" -name '*.dylib' -type f \
    -exec chmod 0644 {} + 2>/dev/null || true

# Dropbear host key — must be readable only by root
chmod 0600 "${STAGING}/etc/dropbear/dropbear_rsa_host_key"
chmod 0600 "${STAGING}/etc/dropbear/dropbear_ed25519_host_key"

# ---------------------------------------------------------------------------
# Pack
# ---------------------------------------------------------------------------
rm -f "${ENTS_MOUNTFS}" "${ENTS_RESTORED}" "${ENTS_DISK}" "${ENTS_PRIV}" "${ENTS_BASIC}"

rm -rf "${STAGING}/var"
mkdir -p "${STAGING}/private"
mv "${STAGING}/etc" "${STAGING}/private/etc"

log "Creating ${OUTPUT}"
tar --uname root --uid 0 \
    --gname wheel --gid 0 \
    -czf "${OUTPUT}" -C "${STAGING}" .

# rm -rf .binpack
log "Done → ${OUTPUT}"
