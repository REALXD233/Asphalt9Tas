Set-StrictMode -Version Latest

function Assert-A9TasElfLoadAlignment {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ReadElf,

        [Parameter(Mandatory = $true)]
        [string]$ElfPath,

        [Parameter(Mandatory = $true)]
        [UInt64]$ExpectedAlignment,

        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    $programHeaderLines = @(& $ReadElf '-lW' $ElfPath)
    if ($LASTEXITCODE -ne 0) {
        throw "$Label program-header inspection failed"
    }

    $loadAlignments = @()
    foreach ($line in $programHeaderLines) {
        if ($line -match '^\s*LOAD\s+.*\s+(0x[0-9a-fA-F]+)\s*$') {
            $loadAlignments += [Convert]::ToUInt64($Matches[1].Substring(2), 16)
        }
    }
    if ($loadAlignments.Count -eq 0) {
        throw "$Label has no ELF LOAD segments"
    }
    foreach ($alignment in $loadAlignments) {
        if ($alignment -ne $ExpectedAlignment) {
            throw ("{0} LOAD alignment mismatch: expected=0x{1:X} actual=0x{2:X}" -f `
                   $Label, $ExpectedAlignment, $alignment)
        }
    }
}
