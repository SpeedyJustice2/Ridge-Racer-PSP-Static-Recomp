# Round 4: hires frames (what the window shows) at internal scale 2, base vs old.
$configs = @(
    @{ name = "base"; env = @{ RR_SHOT_HIRES = "1" }; args = @("--internal-scale","2") },
    @{ name = "old";  env = @{ RR_SHOT_HIRES = "1"; RR_MIRROR_OLD = "1" }; args = @("--internal-scale","2") }
)
