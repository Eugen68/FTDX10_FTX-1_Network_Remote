@echo off
setlocal
rem ============================================================
rem  FTX-1 Companion - scriere firmware pe ESP32-S3 (portul COM)
rem  Pune acest fisier in acelasi folder cu:
rem    ftx1_companion_ota.bin, ota_data_initial.bin
rem    (si ftx1_companion-merged.bin pentru o placa noua)
rem ============================================================
cd /d "%~dp0"

echo.
echo  FTX-1 Companion - scriere firmware
echo  ----------------------------------
echo  Inainte: inchide Tera Term si scoate cablul spre radio din portul USB.
echo  Placa se leaga la PC doar pe portul COM.
echo.

rem --- Python si esptool -------------------------------------------------
set "PY=python"
where python >nul 2>nul || set "PY=py"
%PY% --version >nul 2>nul
if errorlevel 1 (
    echo [EROARE] Python nu este instalat. Instaleaza-l de pe https://www.python.org/downloads/
    echo          si bifeaza "Add python.exe to PATH" la instalare.
    goto :end
)
%PY% -m esptool version >nul 2>nul
if errorlevel 1 (
    echo esptool lipseste, il instalez...
    %PY% -m pip install --upgrade esptool
    if errorlevel 1 (
        echo [EROARE] Nu am putut instala esptool.
        goto :end
    )
)

rem --- Portul: cautat automat dupa chipul USB-serial al placii (WCH CH343 / CH340, VID 1A86) ----------
rem Porturile Silicon Labs (VID 10C4) sunt ale radioului (CAT) si nu se aleg niciodata automat:
rem resetul esptool misca DTR/RTS, care pe portul Standard al radioului poate porni emisia.
set "PORT="
set "NFOUND=0"
for /f "usebackq delims=" %%P in (`powershell -NoProfile -Command "Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -match 'VID_1A86' -and $_.Name -match '\((COM\d+)\)' } | ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } }"`) do (
    set /a NFOUND+=1
    set "PORT=%%P"
)

if "%NFOUND%"=="1" (
    echo Placa gasita pe %PORT% ^(chip WCH CH343/CH340^).
    goto :port_ok
)

echo Porturi seriale gasite:
powershell -NoProfile -Command "Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match '\(COM\d+\)' } | ForEach-Object { '   ' + $_.Name }"
echo.
if "%NFOUND%"=="0" (
    echo Nu am gasit automat placa ^(niciun port WCH CH343/CH340^).
) else (
    echo Am gasit %NFOUND% porturi WCH: alege-l pe cel al placii.
)
echo NU alege un port "Silicon Labs CP210x": acela este al radioului.
set "PORT="
set /p "PORT=Scrie portul placii (de exemplu COM30): "
if "%PORT%"=="" (
    echo [EROARE] Nu ai ales niciun port.
    goto :end
)

:port_ok
rem --- Ce scriem -------------------------------------------------------------
echo.
echo  1 = Actualizare, PASTREAZA Wi-Fi-ul si parolele (recomandat)
echo  2 = Placa noua / resetare completa (STERGE setarile, apoi meniul serial)
set "MODE=1"
set /p "MODE=Alege 1 sau 2 (Enter = 1): "

if "%MODE%"=="2" goto :full

if not exist "ftx1_companion_ota.bin" (
    echo [EROARE] Lipseste ftx1_companion_ota.bin in acest folder.
    goto :end
)
if not exist "ota_data_initial.bin" (
    echo [EROARE] Lipseste ota_data_initial.bin in acest folder.
    goto :end
)
echo.
echo Scriu aplicatia pe %PORT% (setarile raman)...
%PY% -m esptool --chip esp32s3 -p %PORT% -b 460800 write-flash 0xf000 ota_data_initial.bin 0x20000 ftx1_companion_ota.bin
goto :result

:full
if not exist "ftx1_companion-merged.bin" (
    echo [EROARE] Lipseste ftx1_companion-merged.bin in acest folder.
    goto :end
)
echo.
echo ATENTIE: setarile Wi-Fi si parolele vor fi sterse.
set "OK=n"
set /p "OK=Continui? (d/n): "
if /i not "%OK%"=="d" goto :end
echo Scriu firmware-ul complet pe %PORT%...
%PY% -m esptool --chip esp32s3 -p %PORT% -b 460800 write-flash 0x0 ftx1_companion-merged.bin

:result
if errorlevel 1 (
    echo.
    echo [EROARE] Scrierea nu a reusit.
    echo  - Verifica portul COM si ca Tera Term este inchis.
    echo  - Daca scrie "Failed to connect": tine apasat BOOT, apasa scurt RST,
    echo    elibereaza BOOT si ruleaza din nou acest fisier.
) else (
    echo.
    echo [OK] Gata. Apasa RST pe placa, pune la loc cablul spre radio
    echo      si reincarca pagina in browser cu Ctrl+F5.
)

:end
echo.
pause
endlocal
