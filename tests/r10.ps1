# Round 10: texture on/off timeline for 20 s of game time after GO, 60 fps reference vs 120 fps.
$configs = @(
    @{ name = "f60";  fps = @(); env = @{ RR_TEXTIME = "4600,1300"; RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "1200" } },
    @{ name = "f120"; env = @{ RR_TEXTIME = "4600,2600"; RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "2400" } }
)
