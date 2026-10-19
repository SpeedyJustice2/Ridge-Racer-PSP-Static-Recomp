# r17: intro pan speed. PANLOG records the intro camera clock / timeline / shot per call at 60, 90, 120, 180 fps.
$common = @{ RR_GO_SHOTS = "0,1"; RR_PANLOG = "1" }
$configs = @(
    @{ name = "ref60";  fps = @("--internal-scale","3");                                                   env = @{ RR_GO_STOP = "300" } },
    @{ name = "fps120"; fps = @("--fps","120","--fps-patch","car","--no-low-latency","--internal-scale","3"); env = @{ RR_GO_STOP = "600" } },
    @{ name = "fps90";  fps = @("--fps","90","--fps-patch","car","--no-low-latency","--internal-scale","3");  env = @{ RR_GO_STOP = "450" } },
    @{ name = "fps180"; fps = @("--fps","180","--fps-patch","car","--no-low-latency","--internal-scale","3"); env = @{ RR_GO_STOP = "900" } }
)
