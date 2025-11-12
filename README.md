# node_features_cpuinfo

A Slurm execution node can have an arbitrary comma-separated list of *feature strings* associated with it in the configuration.  These strings can be used to establish *constraints* on the set of nodes on which a job can execute.

On Linux the `/proc/cpuinfo` file defines many of the features that jobs are likely to use:  processor vendor and model, cache size, and ISA extensions.  To that end, the `node_features/cpuinfo` plugin is designed to pull features from that file and return them to the `slurmctld` to augment the statically-configured list of features.


## Synthesized features

All features synthesized by the plugin are formatted as **``TYPE::VALUE``**.  The possible **``TYPE``** values are:

| Type     | Description                                             |
| -------- | ------------------------------------------------------- |
| `VENDOR` | CPU vendor name (e.g. `GenuineIntel` or `AuthenticAMD`) |
| `MODEL`  | succinct CPU model name extracted from the verbose name |
| `CACHE`  | Kilobytes of cache reported by the CPU                  |
| `ISA`    | Available ISA extensions (e.g. `avx512f` or `sse4_1`    |

For a user to submit a job that requires the AVX512 Byte-Word and AVX512 Foundational ISA extensions, the command might look like:

```bash
[PROMPT]$ sbatch … --constrain='ISA::avx512f&ISA::avx512bw' …
```

The syntax for using multiple features in a constraint are documented in the `sbatch` man page.


## Building

The project includes a CMakeLists.txt file that makes the build simpler:

```bash
[PROMPT]$ mkdir build-2025.11.12
[PROMPT]$ cd build-2025.11.12
[PROMPT]$ cmake -DSLURM_PREFIX=/opt/shared/slurm/current \
                -DSLURM_SOURCE_DIR=/opt/shared/slurm/current/src \
                -DSLURM_BUILD_DIR=/opt/shared/slurm/current/build-2025.10.14 \
                -DCMAKE_BUILD_TYPE=Release \
                ..
-- The C compiler identification is GNU 4.8.5
-- Detecting C compiler ABI info
-- Detecting C compiler ABI info - done
-- Check for working C compiler: /usr/bin/cc - skipped
-- Detecting C compile features
-- Detecting C compile features - done
-- Found SLURM: /opt/shared/slurm/current/lib/libslurm.so  
-- Configuring done (0.4s)
-- Generating done (0.0s)
-- Build files have been written to: /home/1001/node_features_cpuinfo/build

[PROMPT]$ make
[ 25%] Building C object CMakeFiles/node_features_cpuinfo.dir/node_features_cpuinfo.c.o
[ 50%] Linking C shared module node_features_cpuinfo.so
[ 50%] Built target node_features_cpuinfo
[ 75%] Building C object CMakeFiles/node_features_cpuinfo_test.dir/node_features_cpuinfo.c.o
[100%] Linking C executable node_features_cpuinfo_test
[100%] Built target node_features_cpuinfo_test
```

The test program can be used to confirm against the system's `/proc/cpuinfo` or any of the example files in the `docs/` directory:

```bash
[PROMPT]$ ./node_features_cpuinfo_test /proc/cpuinfo ../docs/cpuinfo.gen3+gpu 
/proc/cpuinfo:    VENDOR::GenuineIntel,MODEL::E5-2695_v4,CACHE::46080KB,ISA::sse,ISA::sse2,ISA::sse4_1,ISA::sse4_2,ISA::avx,ISA::avx2
../docs/cpuinfo.gen3+gpu:    VENDOR::AuthenticAMD,MODEL::EPYC_7502,CACHE::512KB,ISA::sse,ISA::sse2,ISA::sse4_1,ISA::sse4_2,ISA::avx,ISA::avx2
```

The plugin can be installed but will not be loaded into `slurmctld` or `slurmd` until the Slurm configuration has been modified:

```bash
[PROMPT]$ make install
[ 50%] Built target node_features_cpuinfo
[100%] Built target node_features_cpuinfo_test
Install the project...
-- Install configuration: "Release"
-- Installing: /opt/shared/slurm/current/lib/slurm/node_features_cpuinfo.so
```


## Configuration changes

The `NodeFeaturesPlugins` property must have this plugin added to it in the `slurm.conf` file.  For example, if no such plugins have been enabled to this point:

```
   :
NodeFeaturesPlugins=node_features/cpuinfo
   :
```

Once enabled, the synthesized features the execution node's `slurmd` returns will augment any features configured statically in the `slurm.conf` file:

```bash
[PROMPT]$ cat /etc/slurm/slurm.conf
   :
NodeName=n[000-002,004-007,009-013] … Feature=Gen1 …
   :

[PROMPT]$ scontrol show node n013 | grep Features
   AvailableFeatures=Gen1,VENDOR::GenuineIntel,MODEL::E5530,CACHE::8192KB,ISA::sse,ISA::sse2,ISA::sse4_1,ISA::sse4_2
   ActiveFeatures=Gen1,VENDOR::GenuineIntel,MODEL::E5530,CACHE::8192KB,ISA::sse,ISA::sse2,ISA::sse4_1,ISA::sse4_2
```
