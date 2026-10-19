# Round 12: every frame around one billboard picture change (fade), 60 vs 120. Hires at internal scale 2.
$configs = @(
    @{ name = "f60";  fps = @(); env = @{ RR_SHOT_HIRES = "1"; RR_SHOT_RANGE = "4860,70";  RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "240" }; args = @("--internal-scale","2") },
    @{ name = "f120"; env = @{ RR_SHOT_HIRES = "1"; RR_SHOT_RANGE = "5030,130"; RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "460" }; args = @("--internal-scale","2") }
)
