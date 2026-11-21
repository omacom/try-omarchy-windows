#!/bin/bash
# deskbench.sh start|stop [NAME] (root): records page flips (vblank events of
# the virtio-gpu output) and trace markers; stop writes out/NAME.trace.
T=/sys/kernel/tracing
case "$1" in
start)
    echo 0 > $T/tracing_on
    echo > $T/trace
    echo 65536 > $T/buffer_size_kb
    echo 1 > $T/events/drm/drm_vblank_event_delivered/enable
    chmod 755 $T; chmod 666 $T/trace_marker
    echo 1 > $T/tracing_on
    echo "tracing" ;;
stop)
    echo 0 > $T/tracing_on
    cp $T/trace ${OUT:-/tmp}/${2:-desk}.trace
    echo 0 > $T/events/drm/drm_vblank_event_delivered/enable
    echo "$(grep -c vblank_event ${OUT:-/tmp}/${2:-desk}.trace) flips in out/${2:-desk}.trace" ;;
esac
