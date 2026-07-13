echo "======================== Trace 1 =========================="
./kvTraceGenerator /mnt/nvme/data/kvcache_traces_1.csv 127.0.0.1 6363 -1 12
sleep 5
echo "======================== Trace 2 =========================="
./kvTraceGenerator /mnt/nvme/data/kvcache_traces_2.csv 127.0.0.1 6363 -1 12
sleep 5
echo "======================== Trace 3 =========================="
./kvTraceGenerator /mnt/nvme/data/kvcache_traces_3.csv 127.0.0.1 6363 -1 12
sleep 5
echo "======================== Trace 4 =========================="
./kvTraceGenerator /mnt/nvme/data/kvcache_traces_4.csv 127.0.0.1 6363 -1 12
sleep 5
echo "======================== Trace 5 =========================="
./kvTraceGenerator /mnt/nvme/data/kvcache_traces_5.csv 127.0.0.1 6363 -1 12
