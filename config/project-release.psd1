@{
    ReleaseId = 'cs-streamline-2.14.1-4'
    ReleaseTag = 'cs-streamline-v2.14.1-4'
    KeyId = 1
    PublicKeyXY = '78be6901836d3a547e359a3407387a19bce9efa1ff54486e62bc330d64fcf95d45d50e8f81e993abfdd7de2bdf4574ada03f4883ee7f327a741ba1b9f1e0cfc6'
    Upstream = @{
        Version = 'v2.14.1'
        Url = 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.1/streamline-sdk-v2.14.1.zip'
        Sha256 = '92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b'
    }
    AmdSdk = @{
        Commit = '60f4ea81909200d8542eca14dccb2628b763a9a3'
        Patch = 'patches/fidelityfx-sdk-2.3.0-fg-completion.patch'
    }
    Runtime = @(
        @{ Name = 'amd_fidelityfx_upscaler_dx12.dll'; SourcePath = 'external/fidelityfx-sdk/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll'; Role = 'AmdModule'; Sha256 = 'd0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46' }
        @{ Name = 'amd_fidelityfx_framegeneration_dx12.dll'; SourcePath = 'external/fidelityfx-sdk/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll'; Role = 'AmdModule'; Sha256 = '02297beedd285e822d3a64f314cf00faf378dcec0edc47ff0c4dd71b3a8c2f18' }
        @{ Name = 'cs_fidelityfx_upscaler_dx12.dll'; BuildProject = 'cs_fidelityfx_upscaler_dx12'; SymbolName = 'cs_fidelityfx_upscaler_dx12.pdb'; Role = 'ProjectVendorModule' }
        @{ Name = 'cs_fidelityfx_framegeneration_dx12.dll'; BuildProject = 'cs_fidelityfx_framegeneration_dx12'; SymbolName = 'cs_fidelityfx_framegeneration_dx12.pdb'; Role = 'ProjectVendorModule' }
        @{ Name = 'nvngx_dlss.dll'; ArchivePath = 'bin/x64/nvngx_dlss.dll'; Role = 'NvidiaModule'; Sha256 = '3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983' }
        @{ Name = 'nvngx_dlssg.dll'; ArchivePath = 'bin/x64/nvngx_dlssg.dll'; Role = 'NvidiaModule'; Sha256 = 'ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82' }
        @{ Name = 'sl.common.dll'; BuildProject = 'sl.common'; Role = 'ProjectCommon' }
        @{ Name = 'sl.dlss.dll'; ArchivePath = 'bin/x64/sl.dlss.dll'; Role = 'NvidiaModule'; Sha256 = '73bf52c0cfaa5900a8f3f4a91306e4625e7cca696dfb305aae44c9f97b582e1f' }
        @{ Name = 'sl.dlss_g.dll'; ArchivePath = 'bin/x64/sl.dlss_g.dll'; Role = 'NvidiaModule'; Sha256 = 'f4a6b2b14dcc0b1485989e430d3b4e3a44ac1800b92ba1ad74f476e64fb2b09c' }
        @{ Name = 'sl.fsr.dll'; BuildProject = 'sl.fsr'; Role = 'ProjectPlugin' }
        @{ Name = 'sl.fsr_g.dll'; BuildProject = 'sl.fsr_g'; Role = 'ProjectPlugin' }
        @{ Name = 'sl.interposer.dll'; BuildProject = 'sl.interposer'; Role = 'ProjectInterposer' }
        @{ Name = 'sl.pcl.dll'; ArchivePath = 'bin/x64/sl.pcl.dll'; Role = 'NvidiaModule'; Sha256 = 'f13d51cfa05f4cd514df2026049e2db8adf359221713170ad386fd499915b582' }
        @{ Name = 'sl.reflex.dll'; ArchivePath = 'bin/x64/sl.reflex.dll'; Role = 'NvidiaModule'; Sha256 = '0ce9725e3e03ea9e7f81d008b57f33ee365973d2e349131c8b1c3e3378fe2db0' }
    )
    Licenses = @(
        @{ ArchivePath = 'license.txt'; OutputPath = 'license.txt' }
        @{ ArchivePath = 'bin/x64/nvngx_dlss.license.txt'; OutputPath = 'bin/x64/nvngx_dlss.license.txt' }
        @{ ArchivePath = 'bin/x64/reflex.license.txt'; OutputPath = 'bin/x64/reflex.license.txt' }
        @{ SourcePath = 'external/fidelityfx-sdk/docs/license.md'; OutputPath = 'amd-fidelityfx-license.md' }
        @{ SourcePath = 'external/fidelityfx-sdk/3rdpartynotice.md'; OutputPath = 'amd-fidelityfx-third-party-notices.md' }
    )
}
