#!/bin/bash
# given the center frequency of a HFDL band, start dumphfdl for that band with a specific list of channels
# KA9Q May 2025
source /etc/radio/hfdl.conf

if [ $# -lt 1 ]; then
    echo "Usage: $0 <band_center> [args-to-dumphfdl....]"
    exit 1
fi

center="$1"
case "$center" in
    21964k)
	FREQS=(21928.0 21931.0 21934.0 21937.0 21949.0 21955.0 21982.0 21990.0 21997.0)
	;;

    17944k)
	FREQS=(17901. 17912. 17916. 17919. 17922. 17928. 17934. 17958. 17967. 17985.)
	;;

    15025k)
	FREQS=(15025.)
	;;

    13310k)
	FREQS=(13264. 13270. 13276. 13303. 13312. 13315. 13321. 13324. 13342. 13351. 13354.)
	;;

    11287k)
	FREQS=(11184. 11306. 11312. 11318. 11321. 11327. 11348. 11354. 11384. 11387.)
	;;

    10061k5)
	FREQS=(10027. 10030. 10060. 10063. 10066. 10075. 10081. 10084. 10087. 10093.)
	;;

    8902k5)
	FREQS=(8825. 8834. 8843. 8885. 8886. 8894. 8912. 8921. 8927. 8936. 8939. 8942. 8948. 8957. 8977.)
	;;

    6622k)
	FREQS=(6529. 6532. 6535. 6559. 6565. 6589. 6596. 6619. 6628. 6646. 6652. 6661. 6712.)
	;;

    5587k)
	FREQS=(5451. 5502. 5508. 5514. 5529. 5538. 5544. 5547. 5583. 5589. 5622. 5652. 5655. 5720.)
	;;

    4672k)
	FREQS=(4654. 4660. 4681. 4687.)
	;;

    3477k)
	FREQS=(3455. 3497.)
	;;

    2980k)
	FREQS=(2941. 2944. 2992. 2998. 3007. 3016.)
	;;

    *)
	echo "Invalid channel center $center"
	exit 1
	;;
esac


exec_cmd="${DUMPHFDL} --station-id \"\$d\" --output decoded:json:tcp:address=feed.airframes.io,port=5556 --output decoded:json:file:path=${LOG} --iq-file - --sample-rate \"\$r\" --centerfreq \"\$k\"  --sample-format cs16 --system-table=${SYSTABLE} --system-table-save=${SYSTABLE} \"\$k\" ${FREQS[@]}"

cmd=(/usr/local/bin/pcmrecord --raw -v --exec "$exec_cmd" "${MCAST}")

"${cmd[@]}"
