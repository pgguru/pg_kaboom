# TODO

## weapons
- shmem corruption
- clog truncation/corruption
- random system backend killing
- chmod 000 on random files
- checksum invalidation
- other random page corruption
- delayed WAL application
- backend memory allocation - specific sizes, and progressive allocation in different contexts until OOM
- more!

## testing
- get some testing going
  - hard to test failures for failing the right way, but come up with ways to do this safely
  - to test fill_log, etc, without hurting host machine will need to setup loopback filesystems
- can this be done inside the Pg `make check` target, or do we need our own custom harness?
