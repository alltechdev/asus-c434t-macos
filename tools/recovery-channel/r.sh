#!/bin/sh
# Poll server.py for numbered commands, run each once, upload its output.
U=@URL@
n=1
echo "connected to $U - leave this window open (Ctrl-C to stop)"
while :; do
  if curl -sf -o /tmp/c.sh $U/c/$n; then
    echo "running command $n"
    sh /tmp/c.sh > /tmp/o.txt 2>&1
    curl -s -T /tmp/o.txt $U/o/$n > /dev/null
    n=$((n + 1))
  else
    sleep 2
  fi
done
