# Round 7: billboard brightness over game time, 120 fps vs the real 60 fps game. Region log relative to GO (frames).
$configs = @(
    @{ name = "f120"; env = @{ RR_REGION_GO = "225,110,50,35;0,1200"; RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "1200" } },
    @{ name = "f60";  env = @{ RR_REGION_GO = "225,110,50,35;0,600";  RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "600" }; args = @("--fps","60") },
    @{ name = "old";  env = @{ RR_REGION_GO = "225,110,50,35;0,1200"; RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "1200"; RR_MIRROR_OLD = "1" } }
)
