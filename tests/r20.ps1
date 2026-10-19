# r20: where does the GE prim time go (setup / vertices / assembly)? RR_PROF=1, 240 and 144 fps, internal scale 3.
$common = @{ RR_PROF = "1" }
$configs = @(
    @{ name = "fps240_s3"; fps = @("--fps","240","--fps-patch","car","--no-low-latency","--internal-scale","3") },
    @{ name = "fps144_s3"; fps = @("--fps","144","--fps-patch","car","--no-low-latency","--internal-scale","3") }
)
