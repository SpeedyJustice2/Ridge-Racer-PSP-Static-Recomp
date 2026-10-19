# r23: 240 fps at internal scale 8: what costs GPU time? MSAA level and anisotropic filtering.
$common = @{ }
$b = @("--fps","240","--fps-patch","car","--no-low-latency","--internal-scale","8","--latency-log")
$configs = @(
    @{ name = "s8_msaa4";       fps = $b; args = @("--msaa","4") },
    @{ name = "s8_msaa1";       fps = $b; args = @("--msaa","1") },
    @{ name = "s8_msaa1_aniso1"; fps = $b; args = @("--msaa","1","--aniso","1") },
    @{ name = "s8_msaa8_aniso1"; fps = $b; args = @("--aniso","1") }
)
