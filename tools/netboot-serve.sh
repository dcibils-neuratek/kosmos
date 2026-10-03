#!/bin/bash
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# The boot server the M700 network-boots Kosmos from (`roadmap.md`, the M700
# booted over the network; `make netboot` lays out what it serves).
#
# **dnsmasq as a proxy**: it answers only machines asking to boot from the
# network, and hands out no addresses - the router keeps doing that, and
# nothing else on the network notices. The machine's firmware gets its
# address from the router and, from this, where to fetch `bootx64.efi`;
# Kosmos's loader then fetches the kernel beside it, from the same TFTP.
#
# Run in a Terminal of your own: it asks for your password, because DHCP,
# TFTP and PXE listen on ports only root may open. It stays in the
# foreground and prints every request it answers; Control-C stops it.
#
#   bash tools/netboot-serve.sh [FOLDER]        (build/netboot by default)

set -u

folder="${1:-build/netboot}"
root="$(cd "$(dirname "$0")/.." && pwd)"

case "$folder" in
    /*) ;;
    *) folder="$root/$folder" ;;
esac

if [ ! -f "$folder/bootx64.efi" ] || [ ! -f "$folder/boot/kosmos.bin" ]; then
    echo "netboot-serve: nothing to serve in $folder - run \`make netboot\` first"
    exit 1
fi

dnsmasq="$(command -v dnsmasq || echo /opt/homebrew/sbin/dnsmasq)"

if [ ! -x "$dnsmasq" ]; then
    echo "netboot-serve: no dnsmasq - \`brew install dnsmasq\`"
    exit 1
fi

# The interface the Mac reaches the router through, and its network.
interface="$(route -n get default 2>/dev/null | awk '/interface:/ { print $2 }')"
address="$(ipconfig getifaddr "$interface" 2>/dev/null)"
mask="$(ipconfig getoption "$interface" subnet_mask 2>/dev/null)"

if [ -z "$interface" ] || [ -z "$address" ] || [ -z "$mask" ]; then
    echo "netboot-serve: this Mac is not on a network it can name"
    exit 1
fi

network="$(python3 -c "import ipaddress,sys; print(ipaddress.ip_network(sys.argv[1] + '/' + sys.argv[2], strict=False).network_address)" "$address" "$mask")"

echo "netboot-serve: serving $folder"
echo "netboot-serve: on $interface, this Mac at $address, the network $network/$mask"
echo "netboot-serve: boot the M700 from the network now (F12, then the network entry)"
echo

# Both of the codes a 64-bit UEFI firmware may say it is: 7 (EFI BC) and 9
# (EFI x86-64). The file is named whole, so dnsmasq adds no ".0" to it.
exec sudo "$dnsmasq" --no-daemon --conf-file=/dev/null --port=0 \
    --interface="$interface" --bind-interfaces \
    --dhcp-range="$network,proxy,$mask" \
    --pxe-service=BC_EFI,"Kosmos",bootx64.efi \
    --pxe-service=X86-64_EFI,"Kosmos",bootx64.efi \
    --enable-tftp --tftp-root="$folder" \
    --log-dhcp --log-facility=-
