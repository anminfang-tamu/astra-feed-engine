# astra-feed-engine

AstraFeed: Low-Latency C++ Market Data Feed Handler

Sender: ASTRA_CPU_A=3 ASTRA_CPU_B=4 ./scripts/run_itch_ab_senders.sh
Engine: ASTRA_LATENCY_METRICS=0 ASTRA_CPU=2 ASTRA_UDP_RX=recvmmsg ASTRA_UDP_BATCH_SIZE=64 \
 ./build/md_engine 127.0.0.1 9000 127.0.0.1 9001
