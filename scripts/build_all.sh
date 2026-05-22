set -e

MODULE_NAME="relm"

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
KOBJ="${ROOT_DIR}/${MODULE_NAME}.ko"

REPO_FW="${ROOT_DIR}/lib/firmware/relm"
SYS_FW="/lib/firmware/relm"

cd "${ROOT_DIR}"

echo "[*] Staging firmware files: ${REPO_FW} → ${SYS_FW}"

if [ ! -d "${REPO_FW}" ]; then
    echo "[!] ERROR: repo firmware directory not found: ${REPO_FW}"
    echo "    Create it and add the kernel/initrd:"
    echo "      mkdir -p ${REPO_FW}"
    echo "      cp /boot/vmlinuz-\$(uname -r)    ${REPO_FW}/vmlinuz"
    echo "      cp /boot/initrd.img-\$(uname -r) ${REPO_FW}/initrd.img"
    exit 1
fi

if [ ! -f "${REPO_FW}/vmlinuz" ]; then
    echo "[!] ERROR: guest kernel not found: ${REPO_FW}/vmlinuz"
    echo "    Run: cp /boot/vmlinuz-\$(uname -r) ${REPO_FW}/vmlinuz"
    exit 1
fi

sudo mkdir -p "${SYS_FW}"

echo "    vmlinuz  ($(du -h "${REPO_FW}/vmlinuz" | cut -f1))"
sudo cp "${REPO_FW}/vmlinuz" "${SYS_FW}/vmlinuz"

if [ -f "${REPO_FW}/initrd.img" ]; then
    echo "    initrd.img  ($(du -h "${REPO_FW}/initrd.img" | cut -f1))"
    sudo cp "${REPO_FW}/initrd.img" "${SYS_FW}/initrd.img"
else
    echo "    initrd.img  — not found in repo, skipping (optional)"
    echo "    Note: module will boot without initramfs. If the kernel"
    echo "    needs initrd, run:"
    echo "      cp /boot/initrd.img-\$(uname -r) ${REPO_FW}/initrd.img"
fi

echo "[*] Firmware staged to ${SYS_FW}"
ls -lh "${SYS_FW}/"

echo "[*] Cleaning previous build artifacts"
make clean

echo "[*] Building kernel module"
make

if [ ! -f "${KOBJ}" ]; then
    echo "[!] ERROR: ${MODULE_NAME}.ko was not produced — check build output"
    exit 1
fi

echo "[*] Build successful: ${KOBJ} ($(du -h "${KOBJ}" | cut -f1))"

if lsmod | grep -q "^${MODULE_NAME}\b"; then
    echo "[*] Removing existing module (will call module_exit → VMXOFF)"
    sudo rmmod "${MODULE_NAME}"
    # Give the module exit path time to complete VMXOFF on all CPUs
    sleep 1
fi

echo "[*] Inserting module: sudo insmod ${KOBJ}"
sudo insmod "${KOBJ}"

echo "[*] Module inserted successfully"

echo ""
echo "[*] Kernel log (filtered for '${MODULE_NAME}'):"
echo "────────────────────────────────────────────────────────"
dmesg -T | tail -n 120 | grep -i "\(${MODULE_NAME}\|VCPU\|VMX\|SeaBIOS\|direct.boot\|e820\|bzImage\)" || true
echo "────────────────────────────────────────────────────────"
echo "[*] Done. Run 'dmesg -T | tail -n 200' for full output."
