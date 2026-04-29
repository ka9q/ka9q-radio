#!/usr/bin/env bash
#
#	Horus Binary RTLSDR Helper Script
#
#   Uses ka9q-radio to receive a chunk of spectrum, and passes it into horus_demod.
#


USER=radio

SOURCE=$1
SSRC=$2
TYPE=$3
SAMPRATE=$4
CHANNELS=$5


# Change directory to the horusdemodlib directory.
# If running as a different user, you will need to change this line
#cd /home/$USER/horusdemodlib/


# Receive *centre* frequency, in Hz
# Note: The SDR will be tuned to RXBANDWIDTH/2 below this frequency.
RXFREQ=$SSRC

# Frequency estimator bandwidth. The wider the bandwidth, the more drift and frequency error the modem can tolerate,
# but the higher the chance that the modem will lock on to a strong spurious signal.
# Note: The SDR will be tuned to RXFREQ-RXBANDWIDTH/2, and the estimator set to look at 0-RXBANDWIDTH Hz.
RXBANDWIDTH=10000

# Enable (1) or disable (0) modem statistics output.
# If enabled, modem statistics are written to stats.txt, and can be observed
# during decoding by running: tail -f stats.txt | python fskstats.py
STATS_OUTPUT=0


# Check that the horus_demod decoder has been compiled.
#DECODER=./build/src/horus_demod
#if [ -f "$DECODER" ]; then
#    echo "Found horus_demod."
#else 
#    echo "ERROR - $DECODER does not exist - have you compiled it yet?"
#	exit 1
#fi
DECODER=/usr/local/bin/horus_demod



# Check that bc is available on the system path.
if echo "1+1" | bc > /dev/null; then
    echo "Found bc."
else 
    echo "ERROR - Cannot find bc - Did you install it?"
	exit 1
fi

# Use a local venv if it exists
VENV_DIR=venv
if [ -d "$VENV_DIR" ]; then
    echo "Entering venv."
    source $VENV_DIR/bin/activate
fi

RXFREQ=$SSRC
CONFIG=/etc/radio/horus.cfg
LOG=/var/log/horus.log


# Calculate the frequency estimator limits
FSK_LOWER=-5000
FSK_UPPER=$(echo "$FSK_LOWER + $RXBANDWIDTH" | bc)

echo "SSRC $SSRC: channels=$CHANNELS, rate=$SAMPRATE, SDR Centre Frequency: $RXFREQ Hz, FSK estimation range: $FSK_LOWER - $FSK_UPPER Hz"

# Start the receive chain.
# Note that we now pass in the SDR centre frequency ($SDR_RX_FREQ) and 'target' signal frequency ($RXFREQ)
# to enable providing additional metadata to Habitat / Sondehub.
# This assumes the payload type for 16-bit PCM in big endian format; should work out the input format parameters
# in a cleaner, more general way
# Probably won't work anyway unless CHANNELS = 2
sox --type raw --encoding signed-integer --bits 16 --endian big    --channels $CHANNELS --rate $SAMPRATE - \
    --type raw --encoding signed-integer --bits 16 --endian little --channels 2         --rate 48000 - \
    | $DECODER -q --stats=5 -g -m binary --fsk_lower=$FSK_LOWER --fsk_upper=$FSK_UPPER - - \
    | python3 -m horusdemodlib.uploader -c /etc/horus.cfg --log $LOG --freq_hz $RXFREQ --freq_target_hz $RXFREQ

echo "SSRC $SSRC exiting"

