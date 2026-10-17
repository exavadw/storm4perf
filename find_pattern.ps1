# Search NSUNS4.exe for mov dword [reg+0x410], 0xffffffff pattern
# This is the ResetRequestedCommand signature: *(this+0x410) = 0xffffffff
$binPath = "C:\dev\agent-bugfix\game\NSUNS4.exe"
$bin = [System.IO.File]::ReadAllBytes($binPath)

# Get PE header to find .text section offset
$peOffset = [BitConverter]::ToInt32($bin, 0x3C)
$numSections = [BitConverter]::ToInt16($bin, $peOffset + 6)
$optHdrSize = [BitConverter]::ToInt16($bin, $peOffset + 20)
$sectionTableOffset = $peOffset + 24 + $optHdrSize

Write-Host "PE sections:"
for ($s = 0; $s -lt $numSections; $s++) {
    $off = $sectionTableOffset + ($s * 40)
    $name = [System.Text.Encoding]::ASCII.GetString($bin, $off, 8).TrimEnd([char]0)
    $vsize = [BitConverter]::ToInt32($bin, $off + 8)
    $vaddr = [BitConverter]::ToInt32($bin, $off + 12)
    $rawsize = [BitConverter]::ToInt32($bin, $off + 16)
    $rawptr = [BitConverter]::ToInt32($bin, $off + 20)
    Write-Host ("  {0}: VA=0x{1:X} VSize=0x{2:X} RawPtr=0x{3:X} RawSize=0x{4:X}" -f $name, $vaddr, $vsize, $rawptr, $rawsize)
}

# Find .text section
$textRawPtr = 0
$textVAddr = 0
$textRawSize = 0
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

Write-Host (".text: RawPtr=0x{0:X} VAddr=0x{1:X} RawSize=0x{2:X}" -f $textRawPtr, $textVAddr, $textRawSize)

# Search for C7 XX 10 04 00 00 FF FF FF FF where XX modrm has mod=10 (disp32)
$count = 0
for ($i = $textRawPtr; $i -lt ($textRawPtr + $textRawSize - 10); $i++) {
    if ($bin[$i] -eq 0xC7) {
        $modrm = $bin[$i+1]
        $mod = ($modrm -shr 6) -band 3
        $rm = $modrm -band 7
        # mod=10 means [reg+disp32], rm!=4 (not SIB)
        if ($mod -eq 2 -and $rm -ne 4) {
            if ($bin[$i+2] -eq 0x10 -and $bin[$i+3] -eq 0x04 -and $bin[$i+4] -eq 0x00 -and $bin[$i+5] -eq 0x00 -and `
                $bin[$i+6] -eq 0xFF -and $bin[$i+7] -eq 0xFF -and $bin[$i+8] -eq 0xFF -and $bin[$i+9] -eq 0xFF) {
                $fileOff = $i
                $rva = $textVAddr + ($i - $textRawPtr)
                $va = 0x140000000 + $rva
                $regBits = ($modrm -shr 3) -band 7
                $baseReg = $rm
                Write-Host ("MATCH: file=0x{0:X} RVA=0x{1:X} VA=0x{2:X} modrm=0x{3:X} reg={4} base={5}" -f $fileOff, $rva, $va, $modrm, $regBits, $baseReg)
                # Show surrounding bytes (32 before, 32 after)
                $start = [Math]::Max(0, $i - 32)
                $end = [Math]::Min($bin.Length, $i + 42)
                $hex = ""
                for ($j = $start; $j -lt $end; $j++) {
                    if ($j -eq $i) { $hex += "[" }
                    $hex += "{0:X2} " -f $bin[$j]
                    if ($j -eq ($i + 9)) { $hex += "] " }
                }
                Write-Host "  Context: $hex"
                $count++
            }
        }
    }
}
Write-Host "Total matches: $count"