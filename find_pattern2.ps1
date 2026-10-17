# Search NSUNS4.exe for multiple ResetRequestedCommand/PreCtrl patterns
$binPath = "C:\dev\agent-bugfix\game\NSUNS4.exe"
$bin = [System.IO.File]::ReadAllBytes($binPath)

# PE header
$peOffset = [BitConverter]::ToInt32($bin, 0x3C)
$numSections = [BitConverter]::ToInt16($bin, $peOffset + 6)
$optHdrSize = [BitConverter]::ToInt16($bin, $peOffset + 20)
$sectionTableOffset = $peOffset + 24 + $optHdrSize

$textRawPtr = 0; $textVAddr = 0; $textRawSize = 0
for ($s = 0; $s -lt $numSections; $s++) {
    $off = $sectionTableOffset + ($s * 40)
    $name = [System.Text.Encoding]::ASCII.GetString($bin, $off, 8).TrimEnd([char]0)
    if ($name -eq ".text") {
        $textRawPtr = [BitConverter]::ToInt32($bin, $off + 20)
        $textVAddr = [BitConverter]::ToInt32($bin, $off + 12)
        $textRawSize = [BitConverter]::ToInt32($bin, $off + 16)
        break
    }
}
Write-Host ".text: RawPtr=0x$($textRawPtr.ToString('X')) VAddr=0x$($textVAddr.ToString('X')) RawSize=0x$($textRawSize.ToString('X'))"

function FileOffToRVA($fileOff) {
    return $textVAddr + ($fileOff - $textRawPtr)
}
function FileOffToVA($fileOff) {
    return 0x140000000 + (FileOffToRVA $fileOff)
}

# Pattern 1: mov edx, 0xcc (BA CC 00 00 00) - memset size argument
Write-Host "`n=== Pattern 1: mov edx, 0xcc (BA CC 00 00 00) ==="
$count1 = 0
for ($i = $textRawPtr; $i -lt ($textRawPtr + $textRawSize - 5); $i++) {
    if ($bin[$i] -eq 0xBA -and $bin[$i+1] -eq 0xCC -and $bin[$i+2] -eq 0x00 -and $bin[$i+3] -eq 0x00 -and $bin[$i+4] -eq 0x00) {
        $rva = FileOffToRVA $i
        $va = FileOffToVA $i
        # Show 20 bytes before and 20 after
        $start = [Math]::Max($textRawPtr, $i - 20)
        $end = [Math]::Min($textRawPtr + $textRawSize, $i + 25)
        $hex = ""
        for ($j = $start; $j -lt $end; $j++) {
            if ($j -eq $i) { $hex += "[" }
            $hex += "$($bin[$j].ToString('X2')) "
            if ($j -eq ($i + 4)) { $hex += "] " }
        }
        Write-Host ("  VA=0x{0:X} RVA=0x{1:X}: {2}" -f $va, $rva, $hex.Trim())
        $count1++
        if ($count1 -ge 30) { Write-Host "  ... (truncated at 30)"; break }
    }
}
Write-Host "Total mov edx,0xcc matches: $count1"

# Pattern 2: lea rcx, [rcx+0x428] = 48 8D 89 28 04 00 00
Write-Host "`n=== Pattern 2: lea rcx,[rcx+0x428] (48 8D 89 28 04 00 00) ==="
$count2 = 0
for ($i = $textRawPtr; $i -lt ($textRawPtr + $textRawSize - 7); $i++) {
    if ($bin[$i] -eq 0x48 -and $bin[$i+1] -eq 0x8D -and $bin[$i+2] -eq 0x89 -and `
        $bin[$i+3] -eq 0x28 -and $bin[$i+4] -eq 0x04 -and $bin[$i+5] -eq 0x00 -and $bin[$i+6] -eq 0x00) {
        $rva = FileOffToRVA $i
        $va = FileOffToVA $i
        $start = [Math]::Max($textRawPtr, $i - 10)
        $end = [Math]::Min($textRawPtr + $textRawSize, $i + 30)
        $hex = ""
        for ($j = $start; $j -lt $end; $j++) {
            if ($j -eq $i) { $hex += "[" }
            $hex += "$($bin[$j].ToString('X2')) "
            if ($j -eq ($i + 6)) { $hex += "] " }
        }
        Write-Host ("  VA=0x{0:X} RVA=0x{1:X}: {2}" -f $va, $rva, $hex.Trim())
        $count2++
        if ($count2 -ge 20) { Write-Host "  ... (truncated at 20)"; break }
    }
}
Write-Host "Total lea rcx,[rcx+0x428] matches: $count2"

# Pattern 3: Any LEA with disp32=0x428 (XX 8D XX 28 04 00 00)
Write-Host "`n=== Pattern 3: any lea reg,[reg+0x428] (* 8D * 28 04 00 00) ==="
$count3 = 0
for ($i = $textRawPtr; $i -lt ($textRawPtr + $textRawSize - 7); $i++) {
    # Check for REX prefix (40-4F) followed by 8D, or just 8D
    $hasRex = ($bin[$i] -ge 0x40 -and $bin[$i] -le 0x4F)
    $leaOff = if ($hasRex) { $i + 1 } else { $i }
    if ($leaOff + 6 -lt ($textRawPtr + $textRawSize) -and $bin[$leaOff] -eq 0x8D) {
        $modrm = $bin[$leaOff + 1]
        $mod = ($modrm -shr 6) -band 3
        if ($mod -eq 2) { # disp32
            $disp = [BitConverter]::ToInt32($bin, $leaOff + 2)
            if ($disp -eq 0x428) {
                $rva = FileOffToRVA $i
                $va = FileOffToVA $i
                $len = if ($hasRex) { 7 } else { 6 }
                $start = [Math]::Max($textRawPtr, $i - 10)
                $end = [Math]::Min($textRawPtr + $textRawSize, $i + $len + 20)
                $hex = ""
                for ($j = $start; $j -lt $end; $j++) {
                    if ($j -eq $i) { $hex += "[" }
                    $hex += "$($bin[$j].ToString('X2')) "
                    if ($j -eq ($i + $len - 1)) { $hex += "] " }
                }
                Write-Host ("  VA=0x{0:X} RVA=0x{1:X}: {2}" -f $va, $rva, $hex.Trim())
                $count3++
                if ($count3 -ge 30) { Write-Host "  ... (truncated at 30)"; break }
            }
        }
    }
}
Write-Host "Total lea *,[*+0x428] matches: $count3"

# Pattern 4: mov dword [reg+0x410], imm32 where imm32=0xFFFFFFFF (any encoding)
# Try: C7 80-8F 10 04 00 00 FF FF FF FF (with SIB too)
Write-Host "`n=== Pattern 4: mov dword [reg+0x410], 0xFFFFFFFF (C7 modrm 10040000 FFFFFFFF) ==="
$count4 = 0
for ($i = $textRawPtr; $i -lt ($textRawPtr + $textRawSize - 10); $i++) {
    if ($bin[$i] -eq 0xC7) {
        $modrm = $bin[$i+1]
        $mod = ($modrm -shr 6) -band 3
        if ($mod -eq 2) { # disp32
            if ($bin[$i+2] -eq 0x10 -and $bin[$i+3] -eq 0x04 -and $bin[$i+4] -eq 0x00 -and $bin[$i+5] -eq 0x00) {
                # Check immediate (after possible SIB byte)
                $rm = $modrm -band 7
                $immOff = if ($rm -eq 4) { $i + 7 } else { $i + 6 } # SIB adds 1 byte
                if ($immOff + 4 -le ($textRawPtr + $textRawSize)) {
                    $imm = [BitConverter]::ToInt32($bin, $immOff)
                    if ($imm -eq -1) { # 0xFFFFFFFF as signed int32
                        $rva = FileOffToRVA $i
                        $va = FileOffToVA $i
                        $end = $immOff + 4
                        $start = [Math]::Max($textRawPtr, $i - 20)
                        $hex = ""
                        for ($j = $start; $j -lt $end; $j++) {
                            if ($j -eq $i) { $hex += "[" }
                            $hex += "$($bin[$j].ToString('X2')) "
                        }
                        $hex += "]"
                        Write-Host ("  VA=0x{0:X} RVA=0x{1:X}: {2}" -f $va, $rva, $hex.Trim())
                        $count4++
                    }
                }
            }
        }
    }
}
Write-Host "Total mov [reg+0x410],0xFFFFFFFF matches: $count4"

# Pattern 5: pxor xmm0,xmm0 (66 0F EF C0) near 0x428 offset - inlined memset(0)
Write-Host "`n=== Pattern 5: pxor xmm0,xmm0 (66 0F EF C0) ==="
$count5 = 0
for ($i = $textRawPtr; $i -lt ($textRawPtr + $textRawSize - 4); $i++) {
    if ($bin[$i] -eq 0x66 -and $bin[$i+1] -eq 0x0F -and $bin[$i+2] -eq 0xEF -and $bin[$i+3] -eq 0xC0) {
        $rva = FileOffToRVA $i
        $va = FileOffToVA $i
        # Only show ones near 0x428-related code (check surrounding 40 bytes for 28 04 00 00)
        $found428 = $false
        $scanStart = [Math]::Max($textRawPtr, $i - 40)
        $scanEnd = [Math]::Min($textRawPtr + $textRawSize, $i + 40)
        for ($k = $scanStart; $k -lt ($scanEnd - 3); $k++) {
            if ($bin[$k] -eq 0x28 -and $bin[$k+1] -eq 0x04 -and $bin[$k+2] -eq 0x00 -and $bin[$k+3] -eq 0x00) {
                $found428 = $true; break
            }
        }
        if ($found428) {
            $start = [Math]::Max($textRawPtr, $i - 20)
            $end = [Math]::Min($textRawPtr + $textRawSize, $i + 30)
            $hex = ""
            for ($j = $start; $j -lt $end; $j++) {
                if ($j -eq $i) { $hex += "[" }
                $hex += "$($bin[$j].ToString('X2')) "
                if ($j -eq ($i + 3)) { $hex += "] " }
            }
            Write-Host ("  VA=0x{0:X} RVA=0x{1:X} (near 0x428): {2}" -f $va, $rva, $hex.Trim())
            $count5++
            if ($count5 -ge 20) { Write-Host "  ... (truncated at 20)"; break }
        }
    }
}
Write-Host "Total pxor xmm0,xmm0 near 0x428 matches: $count5"