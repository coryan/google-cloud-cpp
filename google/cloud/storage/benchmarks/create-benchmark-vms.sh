#!/usr/bin/env bash
# Copyright 2021 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Usage: create-becnhmark-vms.sh [options] zones
#
#   Options:
#     --project=<project-id>     The ID (or number) of the project
#     --vm-name=<name>           The name of the VM created in each zone
#     --vm-type=<mtype>          The VM machine type
#     --image-family=<family>    The VM image family to baseline from
#     -h|--help                  Print this help message
#

set -eu

function print_usage() {
  # Extracts the usage from the file comment starting at line 17.
  sed -n '16,/^$/s/^# \?//p' "$0"
}

# Use getopt to parse and normalize all the args.
PARSED="$(getopt -a \
  --options="h" \
  --longoptions="project:,vm-name:,vm-type:,image-family:,help" \
  --name="$(basename "$0")" \
  -- "$@")"
eval set -- "${PARSED}"

GOOGLE_CLOUD_PROJECT=""
VM_NAME="cloud-cpp-bm-01"
VM_TYPE="c2-standard-16"
IMAGE_FAMILY="cos-stable"
while true; do
  case "$1" in
  --project)
    GOOGLE_CLOUD_PROJECT="$2"
    shift 2
    ;;
  --vm-name)
    VM_NAME="$2"
    shift 2
    ;;
  --vm-type)
    VM_TYPE="$2"
    shift 2
    ;;
  --image-family)
    IMAGE_FAMILY="$2"
    shift 2
    ;;
  -h | --help)
    print_usage
    exit 0
    ;;
  --)
    shift
    break
    ;;
  esac
done

if [[ $# -eq 0 ]]; then
  echo "No zones specified"
  print_usage
  exit 1
fi

ZONES=("${@}")

SOURCE_IMAGE=$(gcloud compute images list --filter="family:${IMAGE_FAMILY}" --format='value(name)')
readonly SOURCE_IMAGE
SOURCE_IMAGE_PROJECT=$(gcloud compute images list --filter="family:${IMAGE_FAMILY}" --format='value(selfLink)' |
  sed -e 's;^.*projects/;;' -e 's;/.*;;')
readonly SOURCE_IMAGE_PROJECT
SOURCE_IMAGE_FEATURES=$(gcloud compute images list --filter="family:${IMAGE_FAMILY}" --format='value(guestOsFeatures)')
readonly SOURCE_IMAGE_FEATURES

IMAGE="${SOURCE_IMAGE}-gvnic"
IMAGE_PROJECT="${GOOGLE_CLOUD_PROJECT}"
if [[ "${SOURCE_IMAGE_FEATURES}" =~ "GVNIC" ]]; then
  # The source image already has GVNIC enabled, no need to create a new image.
  IMAGE="${SOURCE_IMAGE}"
  IMAGE_PROJECT="${SOURCE_IMAGE_PROJECT}"
elif gcloud compute images describe "${IMAGE}" \
    --project="${GOOGLE_CLOUD_PROJECT}" >/dev/null 2>&1; then
  echo "vm image (${IMAGE}) already exists"
else
  gcloud compute images create "${IMAGE}" \
    --project="${GOOGLE_CLOUD_PROJECT}" \
    --source-image="${SOURCE_IMAGE}" \
    --source-image-project="${SOURCE_IMAGE_PROJECT}" \
    --guest-os-features=GVNIC
fi

readonly IMAGE
readonly IMAGE_PROJECT

PROJECT_NUMBER="$(gcloud projects describe "${GOOGLE_CLOUD_PROJECT}" --format='value(projectNumber)')"
readonly PROJECT_NUMBER

COMMON_INSTANCE_FLAGS=(
  # Enable Google Direct Access (aka Direct Path) over IPv6
  --private-ipv6-google-access-type=enable-outbound-vm-access

  # Enable higher bandwidth for c2-*-60 VMs, note that this requires `gcloud **beta** compute`
  --network-performance-configs=total-egress-bandwidth-tier=TIER_1

  # Use the premium network tier.
  #  DISABLED --network-tier=PREMIUM

  # This is the recommended network driver to get better performance
  # on GCE VMs.
  --network-interface="nic-type=GVNIC"

  # Create the VM in the desired project
  --project="${GOOGLE_CLOUD_PROJECT}"

  # Configure the VM to use the Google Compute Engine default service account
  --service-account="${PROJECT_NUMBER}-compute@developer.gserviceaccount.com"

  # Use the image created (or found) above, note that this image
  # must support GVNIC, and must have the "GVNIC guest OS feature"
  # enabled.
  --image="${IMAGE}"
  --image-project="${IMAGE_PROJECT}"

  # Use the prescribed VM machine type
  --machine-type="${VM_TYPE}"

  # We will fetch the ssh keys using metadata attributes.
  --metadata enable-guest-attributes=TRUE

  # Normal options which should have very little impact on network performance, most
  # of these are cargo-culted from the console.cloud.google.com defaults.
  --maintenance-policy=MIGRATE
  --scopes="https://www.googleapis.com/auth/cloud-platform"
  --boot-disk-size="128GB"
  --boot-disk-type="pd-balanced"
  --no-shielded-secure-boot
  --shielded-vtpm
  --shielded-integrity-monitoring
  --reservation-affinity="any"
)
readonly COMMON_INSTANCE_FLAGS

for zone in "${ZONES[@]}"; do
  if gcloud compute instances describe "${VM_NAME}" \
    --project="${GOOGLE_CLOUD_PROJECT}" \
    --zone="${zone}" >/dev/null 2>&1; then
    echo "vm (${VM_NAME}) already exists in zone ${zone}"
  else
    echo "Creating instance ${VM_NAME} in zone ${zone} (machine type = ${VM_TYPE})"
    gcloud beta compute instances create "${VM_NAME}" \
      --zone="${zone}" \
      "${COMMON_INSTANCE_FLAGS[@]}" \
      --boot-disk-device-name="${VM_NAME}" \
      --metadata-from-file user-data=<(
        cat <<__EOF__
#cloud-config

write_files:
- path: /etc/systemd/network/zz-default.network.d/ipv6.conf
  permissions: 0644
  owner: root
  content: |
    [Network]
    DHCP=yes
    IPv6AcceptRA=yes

- path: /etc/systemd/system/config-firewall.service
  permissions: 0644
  owner: root
  content: |
    [Unit]
    Description=Enable IPerf Port on VM

    [Service]
    Type=oneshot
    RemainAfterExit=true
    ExecStart=/sbin/iptables -A INPUT -p tcp --dport 5001 -j ACCEPT

runcmd:
- systemctl daemon-reload
- systemctl start config-firewall.service
- systemctl restart systemd-networkd
__EOF__
      )
  fi
done

for zone in "${ZONES[@]}"; do
  echo "Fetching host keys ${VM_NAME}"
  id=$(gcloud compute instances describe "${VM_NAME}" \
    --project="${GOOGLE_CLOUD_PROJECT}" \
    --zone="${zone}" \
    --format='value(id)')
  set +e
  delay=1
  for _ in $(seq 1 8); do
    if read -r key < <(gcloud compute instances get-guest-attributes "${VM_NAME}" \
      --project="${GOOGLE_CLOUD_PROJECT}" \
      --zone="${zone}" \
      --query-path=hostkeys/ssh-rsa \
      --format='value(value)'); then
      break
    fi
    delay=$((delay * 2))
    echo "cannot fetch key, trying again in ${delay} seconds"
    sleep $delay
  done
  set -e
  echo "compute.${id} ssh-rsa ${key}" >>"${HOME}/.ssh/google_compute_known_hosts"
  echo "Enabling GCR on ${VM_NAME}"
  gcloud compute ssh --project="${GOOGLE_CLOUD_PROJECT}" --zone="${zone}" "${VM_NAME}" \
    -- -o 'ProxyCommand=corp-ssh-helper %h %p' docker-credential-gcr configure-docker </dev/null || true
done

gcloud compute config-ssh --verbosity=none --project="${GOOGLE_CLOUD_PROJECT}"
