@echo off
rem Signs a file with Azure Artifact Signing (formerly Trusted Signing).  make-release.cmd calls sign-release.cmd (if it
rem exists) with the finished Setup.exe.  To use it:
rem   1. Create an Artifact Signing account in the Azure portal, pass identity validation (individuals: USA/Canada), create a
rem      "Public Trust" certificate profile, and give yourself the "Artifact Signing Certificate Profile Signer" role.
rem   2. Install the signing tools:   winget install -e --id Microsoft.Azure.ArtifactSigningClientTools
rem      and the Azure CLI:          winget install -e --id Microsoft.AzureCLI      then once:  az login
rem   3. Copy this file to sign-release.cmd (it is not published) and fill in the four values below.
rem Signatures are time-stamped, so they stay valid after the short-lived signing certificate expires.
setlocal
set "FILE=%~1"
if "%FILE%"=="" (echo usage: sign-release.cmd ^<file^> & exit /b 1)

rem ---- fill these in ----
set "ENDPOINT=https://eus.codesigning.azure.net"
set "ACCOUNT=your-artifact-signing-account"
set "PROFILE=your-certificate-profile"
rem signtool.exe and Azure.CodeSigning.Dlib.dll (the client tools install both; adjust if yours are elsewhere)
set "SIGNTOOL="
set "DLIB="
rem -----------------------

if "%SIGNTOOL%"=="" for /f "delims=" %%S in ('dir /b /s "%ProgramFiles(x86)%\Windows Kits\10\bin\*signtool.exe" 2^>nul ^| findstr /i "\\x64\\"') do set "SIGNTOOL=%%S"
if "%DLIB%"=="" for /f "delims=" %%D in ('dir /b /s "%LOCALAPPDATA%\Microsoft\MicrosoftArtifactSigningClientTools\Azure.CodeSigning.Dlib.dll" "%ProgramFiles%\Microsoft\ArtifactSigningClientTools\Azure.CodeSigning.Dlib.dll" 2^>nul') do set "DLIB=%%D"
if not exist "%SIGNTOOL%" (echo signtool.exe not found: set SIGNTOOL in sign-release.cmd & exit /b 1)
if not exist "%DLIB%" (echo Azure.CodeSigning.Dlib.dll not found: set DLIB in sign-release.cmd & exit /b 1)
set "META=%TEMP%\rr-signing-metadata.json"
> "%META%" echo {"Endpoint": "%ENDPOINT%", "CodeSigningAccountName": "%ACCOUNT%", "CertificateProfileName": "%PROFILE%", "ExcludeCredentials": ["ManagedIdentityCredential", "WorkloadIdentityCredential", "SharedTokenCacheCredential", "VisualStudioCredential", "VisualStudioCodeCredential", "InteractiveBrowserCredential"]}
"%SIGNTOOL%" sign /v /fd SHA256 /tr "http://timestamp.acs.microsoft.com" /td SHA256 /d "Ridge Racer PSP PC port setup" /dlib "%DLIB%" /dmdf "%META%" "%FILE%"
if errorlevel 1 (echo SIGNING FAILED & exit /b 1)
"%SIGNTOOL%" verify /pa "%FILE%" >nul && echo Signed and verified: %FILE%
