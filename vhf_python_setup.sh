#!/bin/bash                                                                                                                                                                     

set -euo pipefail                                                                                                                                                             
                                                                                                                                                                                  
REPO="https://raw.githubusercontent.com/ringof/rx888-firmware/claude/return-vhf-tuner/vhf"                                                                                      
                                                                                                                                                                                  
mkdir -p rx888_vhf_demo && cd rx888_vhf_demo                                                                                                                                  

echo "Fetching scripts..."
curl -fLO "$REPO/rx888_vhf.py"
curl -fLO "$REPO/vhf_fm_radio.py"

echo "Setting up venv..."
python3 -m venv venv
source venv/bin/activate
pip install pyusb textual

echo ""
echo "Ready. Run:"
echo "  cd rx888_vhf_demo"
echo "  source venv/bin/activate"
echo "  python vhf_fm_radio.py"

