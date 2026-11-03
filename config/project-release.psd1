@{
    Upstream = @{
        Version = 'v2.14.16'
        Url = 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip'
        Sha256 = '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b'
    }
    Runtime = @(
        @{ Name = 'amd_fidelityfx_upscaler_dx12.dll'; SourcePath = 'external/fidelityfx-sdk/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll'; Sha256 = 'd0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46' }
        @{ Name = 'amd_fidelityfx_framegeneration_dx12.dll'; SourcePath = 'external/fidelityfx-sdk/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll'; Sha256 = '02297beedd285e822d3a64f314cf00faf378dcec0edc47ff0c4dd71b3a8c2f18' }
        @{ Name = 'cs_fidelityfx_upscaler_dx12.dll'; BuildProject = 'cs_fidelityfx_upscaler_dx12'; SymbolName = 'cs_fidelityfx_upscaler_dx12.pdb' }
        @{ Name = 'cs_fidelityfx_framegeneration_dx12.dll'; BuildProject = 'cs_fidelityfx_framegeneration_dx12'; SymbolName = 'cs_fidelityfx_framegeneration_dx12.pdb' }
        @{ Name = 'nvngx_dlss.dll'; ArchivePath = 'bin/x64/nvngx_dlss.dll'; Sha256 = 'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e' }
        @{ Name = 'nvngx_dlssg.dll'; ArchivePath = 'bin/x64/nvngx_dlssg.dll'; Sha256 = '8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7' }
        @{ Name = 'sl.common.dll'; BuildProject = 'sl.common' }
        @{ Name = 'sl.dlss.dll'; BuildProject = 'sl.dlss' }
        @{ Name = 'sl.dlss_g.dll'; ArchivePath = 'bin/x64/sl.dlss_g.dll'; Sha256 = '3ed9538df724993bea8a050195a6a78b9c8c8a566d5b05ade477aab3c390e90f' }
        @{ Name = 'sl.fsr.dll'; BuildProject = 'sl.fsr' }
        @{ Name = 'sl.fsr_g.dll'; BuildProject = 'sl.fsr_g' }
        @{ Name = 'sl.interposer.dll'; BuildProject = 'sl.interposer' }
        @{ Name = 'sl.pcl.dll'; BuildProject = 'sl.pcl' }
        @{ Name = 'sl.reflex.dll'; BuildProject = 'sl.reflex' }
    )
    Licenses = @(
        @{ ArchivePath = 'license.txt'; OutputPath = 'license.txt' }
        @{ ArchivePath = 'bin/x64/nvngx_dlss.license.txt'; OutputPath = 'bin/x64/nvngx_dlss.license.txt' }
        @{ ArchivePath = 'bin/x64/reflex.license.txt'; OutputPath = 'bin/x64/reflex.license.txt' }
        @{ SourcePath = 'external/fidelityfx-sdk/docs/license.md'; OutputPath = 'amd-fidelityfx-license.md' }
        @{ SourcePath = 'external/fidelityfx-sdk/3rdpartynotice.md'; OutputPath = 'amd-fidelityfx-third-party-notices.md' }
    )
}
