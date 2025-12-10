#!/bin/bash
#
# Install Voice ACAP to Axis speaker
# Usage: ./install.sh
#

# Configuration
DEVICE="speaker.internal"
USERNAME="nodered"
PASSWORD="rednode"
UPLOAD_URL="http://${DEVICE}/axis-cgi/applications/upload.cgi"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "========================================="
echo "Voice ACAP Installation Script"
echo "========================================="
echo "Target device: ${DEVICE}"
echo "Upload URL: ${UPLOAD_URL}"
echo ""

# Find .eap file
echo "Searching for .eap file..."
EAP_FILE=$(ls -1 Voice_*_armv7hf.eap 2>/dev/null | head -1)

if [ -z "$EAP_FILE" ]; then
    echo -e "${RED}ERROR: No Voice .eap file found in current directory${NC}"
    echo "Expected file pattern: Voice_*_armv7hf.eap"
    exit 1
fi

echo -e "${GREEN}Found: ${EAP_FILE}${NC}"
FILE_SIZE=$(du -h "$EAP_FILE" | cut -f1)
echo "File size: ${FILE_SIZE}"
echo ""

# Upload using curl with digest authentication
echo "Uploading to ${DEVICE}..."
echo ""

RESPONSE=$(curl -s -w "\n%{http_code}" \
    --digest -u "${USERNAME}:${PASSWORD}" \
    -F "packfil=@${EAP_FILE};type=application/octet-stream" \
    "${UPLOAD_URL}")

# Split response into body and status code
HTTP_CODE=$(echo "$RESPONSE" | tail -n 1)
BODY=$(echo "$RESPONSE" | sed '$d')

echo "HTTP Status: ${HTTP_CODE}"
echo ""

if [ "$HTTP_CODE" = "200" ]; then
    echo -e "${GREEN}✓ Upload successful!${NC}"
    echo ""
    echo "Response:"
    echo "$BODY"
    echo ""
    echo -e "${GREEN}Installation complete!${NC}"
    echo ""
    echo "You can now:"
    echo "  • Check status: curl --digest -u ${USERNAME}:${PASSWORD} http://${DEVICE}/local/voice/status"
    echo "  • Test Wyoming: curl --digest -u ${USERNAME}:${PASSWORD} 'http://${DEVICE}/local/voice/test_wyoming?service=piper'"
    echo "  • Test TTS:     curl -X POST -H 'Content-Type: application/json' --digest -u ${USERNAME}:${PASSWORD} 'http://${DEVICE}/local/voice/speak' -d '{\"text\":\"Hej från röstassistenten\"}'"
    echo ""
    exit 0
else
    echo -e "${RED}✗ Upload failed with HTTP status: ${HTTP_CODE}${NC}"
    echo ""
    echo "Response:"
    echo "$BODY"
    echo ""
    exit 1
fi
