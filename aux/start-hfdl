#!/bin/bash
# given the center frequency of a HFDL band, start dumphfdl for that band with a specific list of channels
# KA9Q May 2025
source /etc/radio/hfdl.conf

if [ $# -lt 3 ]; then
    echo "Usage: $0 <band_center> <description> <samprate>"
    exit 1
fi

CENTER="$1"
DESCRIPTION="$2"
SAMPRATE="$3"



case "$CENTER" in
    21964.0000)
	FREQS=(21928. 21931. 21934. 21937. 21949. 21955. 21982. 21990. 21997.)
	;;

    17944.0000)
	FREQS=(17901. 17912. 17916. 17919. 17922. 17928. 17934. 17958. 17967. 17985.)
	;;

    15025.0000)
	FREQS=(15025.)
	;;

    13310.0000)
	FREQS=(13264. 13270. 13276. 13303. 13312. 13315. 13321. 13324. 13342. 13351. 13354.)
	;;

    11287.0000)
	FREQS=(11184. 11306. 11312. 11318. 11321. 11327. 11348. 11354. 11384. 11387.)
	;;

    10061.5000)
	FREQS=(10027. 10030. 10060. 10063. 10066. 10075. 10081. 10084. 10087. 10093.)
	;;

    8902.5000)
	FREQS=(8825. 8834. 8843. 8885. 8886. 8894. 8912. 8921. 8927. 8936. 8939. 8942. 8948. 8957. 8977.)
	;;

    6622.0000)
	FREQS=(6529. 6532. 6535. 6559. 6565. 6589. 6596. 6619. 6628. 6646. 6652. 6661. 6712.)
	;;

    5587.0000)
	FREQS=(5451. 5502. 5508. 5514. 5529. 5538. 5544. 5547. 5583. 5589. 5622. 5652. 5655. 5720.)
	;;

    4672.0000)
	FREQS=(4654. 4660. 4681. 4687.)
	;;

    3477.0000)
	FREQS=(3455. 3497.)
	;;

    2980.0000)
	FREQS=(2941. 2944. 2992. 2998. 3007. 3016.)
	;;

    *)
	echo "Invalid channel center $center"
	sleep 10
	exit 1
	;;
esac

echo "$0: Center frequency = ${CENTER} kHz; Description = \"${DESCRIPTION}\"; Sample rate = ${SAMPRATE} Hz; Channel frequencies (kHz) = ${FREQS[@]}"


${DUMPHFDL} --station-id "${DESCRIPTION}"  --output decoded:json:tcp:address=feed.airframes.io,port=5556 --output decoded:json:file:path="${LOG}" --iq-file - --sample-rate "${SAMPRATE}" --centerfreq "${CENTER}"  --sample-format cs16 --system-table="${SYSTABLE}" --system-table-save="${SYSTABLE}" ${FREQS[@]}
