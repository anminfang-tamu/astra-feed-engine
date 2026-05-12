# NYSE TAQ Integrated Replay

The replay path starts from NYSE TAQ Integrated Book CSV files, including
`.gz` files:

```text
IBF csv.gz -> GzipLineReader -> NyseTaqParser -> MdEvent
           -> NyseTaqReplaySource -> MarketDataMessage -> BookManager
```

Keep downloaded TAQ files under `data/taq/raw/`. Keep generated normalized or
binary replay files under `data/taq/processed/`. Both directories are ignored by
git because the files are large and licensed.

Do not unzip the NYSE `.gz` files for normal replay. `GzipLineReader` streams
them directly.

Prices are parsed as fixed-point ticks with four decimal places:

```text
190.59  -> 1905900
0.125   -> 1250
```

`replay_player` runs the parser and book builder as fast as possible:

```sh
./build/replay_player data/taq/raw/EQY_US_NYSE_IBF_1_YYYYMMDD.gz
./build/replay_player data/taq/raw/EQY_US_NYSE_IBF_1_YYYYMMDD.gz 100000 1
```

Arguments are:

```text
1. file path
2. optional max emitted book messages
3. optional channel id
```

The parser currently maps these TAQ message types:

```text
100 Add Order        -> Add
101 Modify Order     -> Modify
102 Delete Order     -> Delete
103 Order Execution  -> Execution
104 Replace Order    -> Delete old order + Add new order
106 Add Order Refresh-> Add
```

Trade, imbalance, status, and summary records are parsed as `MdEvent` values
but are not emitted into the displayed-book builder yet.

To replay over UDP, run the engine receiver first:

```sh
./build/md_engine 127.0.0.1 9000
```

Then run the TAQ replay sender from another terminal:

```sh
./build/taq_replay_sender data/taq/raw/EQY_US_NYSE_IBF_1_YYYYMMDD.gz 127.0.0.1 9000 100000 1 10000
```

Sender arguments are:

```text
1. file path
2. destination IP
3. destination port
4. optional max emitted book messages
5. optional channel id
6. optional messages per second; omit for max speed
```

The UDP sender emits contiguous wire sequence numbers so the engine's
`SequenceTracker` does not see false gaps from skipped non-book TAQ rows.
