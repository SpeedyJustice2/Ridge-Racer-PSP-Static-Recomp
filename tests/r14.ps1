# Round 14: watch round - real 60 fps game first, then 120 fps, 15 s after GO each.
$configs = @(
    @{ name = "watch60";  fps = @(); env = @{ RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "900" } },
    @{ name = "watch120"; env = @{ RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "1800" } }
)
