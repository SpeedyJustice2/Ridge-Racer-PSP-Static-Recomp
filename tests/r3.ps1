# Round 3: watch round - base then old, 10 s after GO each, no frames saved.
$configs = @(
    @{ name = "base_watch"; env = @{ RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "1200" } },
    @{ name = "old_watch";  env = @{ RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "1200"; RR_MIRROR_OLD = "1" } }
)
