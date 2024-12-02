# Container image

The `Containerfile` in this directory can be used to build an OCI container
image for `b1b`.

### Prerequisites

In order to build the image, the following must exist.

* A compiled `b1b` executable, located at `src/b1b`

* A [`libsavl`](https://github.com/ipilcher/libsavl) RPM, located at
  `oci/libsavl.x86_64.rpm`.

### Building

1. Change to the root directory of this repository (**not** the `oci`
   subdirectory).

1. Build the container image.

   ```
   $ podman build -f oci/Containerfile -t b1b .
   STEP 1/4: FROM fedora:latest
   Resolved "fedora" as an alias (/etc/containers/registries.conf.d/000-shortnames.conf)
   Trying to pull registry.fedoraproject.org/fedora:latest...
   Getting image source signatures
   Copying blob c67300a7d2dc done   |
   Copying config c5c5ba7f65 done   |
   Writing manifest to image destination
   STEP 2/4: RUN --mount=type=bind,source=./oci/,target=/oci/,Z dnf -y install /oci/libsavl.x86_64.rpm libmnl && dnf clean all
   Updating and loading repositories:
    Fedora 41 - x86_64                     100% |  17.5 MiB/s |  35.4 MiB |  00m02s
    Fedora 41 - x86_64 - Updates           100% |   6.9 MiB/s |   5.9 MiB |  00m01s
    Fedora 41 openh264 (From Cisco) - x86_ 100% |  10.2 KiB/s |   6.0 KiB |  00m01s
   Repositories loaded.
   Total size of inbound packages is 50 KiB. Need to download 28 KiB.
   After this operation, 93 KiB extra will be used (install 93 KiB, remove 0 B).
   Package  Arch   Version      Repository        Size
   Installing:
    libmnl  x86_64 1.0.5-6.fc41 fedora        55.2 KiB
    libsavl x86_64 0.7.1-1.fc41 @commandline  38.3 KiB

   Transaction Summary:
    Installing:         2 packages

   [1/1] libmnl-0:1.0.5-6.fc41.x86_64      100% |  48.2 KiB/s |  28.3 KiB |  00m01s
   --------------------------------------------------------------------------------
   [1/1] Total                             100% |  34.6 KiB/s |  28.3 KiB |  00m01s
   Running transaction
   [1/4] Verify package files              100% |   0.0   B/s |   2.0   B |  00m00s
   [2/4] Prepare transaction               100% | 222.0   B/s |   2.0   B |  00m00s
   [3/4] Installing libmnl-0:1.0.5-6.fc41. 100% |  27.6 MiB/s |  56.6 KiB |  00m00s
   [4/4] Installing libsavl-0:0.7.1-1.fc41 100% |   2.4 MiB/s |  39.5 KiB |  00m00s
   Warning: skipped PGP checks for 1 package from repository: @commandline
   Complete!
   Removed 20 files, 12 directories. 0 errors occurred.
   --> 659dfc073260
   STEP 3/4: COPY src/b1b /usr/local/bin/
   --> 4b353d4e5f6a
   STEP 4/4: ENTRYPOINT /usr/local/bin/b1b --stderr --debug
   COMMIT b1b
   --> f712f2bf1ee9
   Successfully tagged localhost/b1b:latest
   f712f2bf1ee909a50e73b97099d9ed3059ab8122da5ae7559fa43c0606447561
   ```

1. Optionally, tag and push the image to an image registry.
