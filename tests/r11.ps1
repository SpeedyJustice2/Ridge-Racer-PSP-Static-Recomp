# Round 11: billboard texture timeline BEFORE GO (intro + countdown), 60 vs 120.
$configs = @(
    @{ name = "f60";  fps = @(); env = @{ RR_TEXTIME = "2400,2400"; RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "60" } },
    @{ name = "f120"; env = @{ RR_TEXTIME = "2400,2400"; RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "60" } }
)
