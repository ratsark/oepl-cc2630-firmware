#!/bin/bash
# Monitor CC2630 tag checkins via AP API for 48 hours
# Usage: ./monitor_48h.sh [mac] [ap_ip]
# Logs to /tmp/tag_monitor.log
# Alerts if tag goes silent for more than 10 minutes

MAC="${1:-00124B00181880B0}"
AP="${2:-192.168.5.4}"
LOG="/tmp/tag_monitor.log"
DURATION=172800  # 48 hours in seconds
CHECK_INTERVAL=300  # Check every 5 minutes
ALERT_THRESHOLD=600  # Alert if silent for 10 minutes

echo "=== CC2630 Tag Monitor ===" | tee "$LOG"
echo "MAC: $MAC" | tee -a "$LOG"
echo "AP: $AP" | tee -a "$LOG"
echo "Started: $(date)" | tee -a "$LOG"
echo "Duration: 48 hours" | tee -a "$LOG"
echo "Check interval: ${CHECK_INTERVAL}s" | tee -a "$LOG"
echo "Alert threshold: ${ALERT_THRESHOLD}s" | tee -a "$LOG"
echo "---" | tee -a "$LOG"

START=$(date +%s)
LAST_SEEN_PREV=0
CHECKIN_COUNT=0
ALERT_COUNT=0

while true; do
    NOW=$(date +%s)
    ELAPSED=$((NOW - START))

    if [ $ELAPSED -ge $DURATION ]; then
        echo "" | tee -a "$LOG"
        echo "=== 48 HOURS COMPLETE ===" | tee -a "$LOG"
        echo "Total checkins observed: $CHECKIN_COUNT" | tee -a "$LOG"
        echo "Alert count: $ALERT_COUNT" | tee -a "$LOG"
        echo "Ended: $(date)" | tee -a "$LOG"
        if [ $ALERT_COUNT -eq 0 ]; then
            echo "RESULT: PASS - Tag ran 48 hours with zero interruptions" | tee -a "$LOG"
        else
            echo "RESULT: FAIL - Tag had $ALERT_COUNT alert(s)" | tee -a "$LOG"
        fi
        exit 0
    fi

    # Query AP
    RESPONSE=$(curl -s "http://$AP/get_db?mac=$MAC" 2>/dev/null)
    if [ $? -ne 0 ] || [ -z "$RESPONSE" ]; then
        echo "[$(date '+%H:%M:%S')] AP unreachable (${ELAPSED}s elapsed)" | tee -a "$LOG"
        sleep $CHECK_INTERVAL
        continue
    fi

    # Parse lastseen from JSON
    LAST_SEEN=$(echo "$RESPONSE" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['tags'][0]['lastseen'])" 2>/dev/null)
    TEMP=$(echo "$RESPONSE" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['tags'][0]['temperature'])" 2>/dev/null)
    BATT=$(echo "$RESPONSE" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['tags'][0]['batteryMv'])" 2>/dev/null)
    RSSI=$(echo "$RESPONSE" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['tags'][0]['RSSI'])" 2>/dev/null)
    REASON=$(echo "$RESPONSE" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['tags'][0]['wakeupReason'])" 2>/dev/null)

    if [ -z "$LAST_SEEN" ]; then
        echo "[$(date '+%H:%M:%S')] Failed to parse AP response" | tee -a "$LOG"
        sleep $CHECK_INTERVAL
        continue
    fi

    AGO=$((NOW - LAST_SEEN))
    HOURS=$((ELAPSED / 3600))
    MINS=$(( (ELAPSED % 3600) / 60 ))

    if [ "$LAST_SEEN" != "$LAST_SEEN_PREV" ]; then
        CHECKIN_COUNT=$((CHECKIN_COUNT + 1))
    fi

    STATUS="OK"
    if [ $AGO -gt $ALERT_THRESHOLD ]; then
        STATUS="ALERT: silent ${AGO}s"
        ALERT_COUNT=$((ALERT_COUNT + 1))
    fi

    printf "[%s] %dh%02dm | lastseen=%ds ago | T=%s BATT=%s RSSI=%s | #%d | %s\n" \
        "$(date '+%H:%M:%S')" "$HOURS" "$MINS" "$AGO" "$TEMP" "$BATT" "$RSSI" "$CHECKIN_COUNT" "$STATUS" \
        | tee -a "$LOG"

    LAST_SEEN_PREV=$LAST_SEEN
    sleep $CHECK_INTERVAL
done
