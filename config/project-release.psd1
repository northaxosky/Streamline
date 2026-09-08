@{
    ReleaseId = 'cs-streamline-2.12.0-1'
    ReleaseTag = 'cs-streamline-v2.12.0-1'
    KeyId = 1
    PublicKeyXY = '78be6901836d3a547e359a3407387a19bce9efa1ff54486e62bc330d64fcf95d45d50e8f81e993abfdd7de2bdf4574ada03f4883ee7f327a741ba1b9f1e0cfc6'
    Upstream = @{
        Version = 'v2.12.0'
        Url = 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.12.0/streamline-sdk-v2.12.0.zip'
        Sha256 = 'f5c0a3d870707dddc3570fb4bcd3655cf48a8a68c3a9d342910cfa21b77dcf48'
    }
    Runtime = @(
        @{ Name = 'nvngx_dlss.dll'; ArchivePath = 'bin/x64/nvngx_dlss.dll'; Role = 'NvidiaModule'; Sha256 = 'be6e434a94ca32499515eb62ca0e6c274526055d568d0426e4c652dcdfb6ee6e' }
        @{ Name = 'nvngx_dlssg.dll'; ArchivePath = 'bin/x64/nvngx_dlssg.dll'; Role = 'NvidiaModule'; Sha256 = '135eaf0733c1e37381a8c28abcf7a862404a54132b81787c04e35d09efc5e36f' }
        @{ Name = 'sl.common.dll'; BuildProject = 'sl.common'; Role = 'ProjectCommon' }
        @{ Name = 'sl.dlss.dll'; ArchivePath = 'bin/x64/sl.dlss.dll'; Role = 'NvidiaModule'; Sha256 = 'a997022d2b93601e0eefc3ddb3067c36df386dd3163ae71e11095191fb14f8e4' }
        @{ Name = 'sl.dlss_g.dll'; ArchivePath = 'bin/x64/sl.dlss_g.dll'; Role = 'NvidiaModule'; Sha256 = '1fec3f8fdfc59d78c4445c276c1a0fb798bf251985f348597dc2b44d0c995e52' }
        @{ Name = 'sl.interposer.dll'; BuildProject = 'sl.interposer'; Role = 'ProjectInterposer' }
        @{ Name = 'sl.pcl.dll'; ArchivePath = 'bin/x64/sl.pcl.dll'; Role = 'NvidiaModule'; Sha256 = '699ab461e64e95189a7fe6a21c79ad237cf56b60ea748cb6c840cd5431ba91d1' }
        @{ Name = 'sl.reflex.dll'; ArchivePath = 'bin/x64/sl.reflex.dll'; Role = 'NvidiaModule'; Sha256 = '7e6e4ccc4b561bd449fb0da90709d9b96b08c3f6f4697362caaa359e72a58a67' }
    )
    Licenses = @(
        @{ ArchivePath = 'license.txt'; OutputPath = 'license.txt' }
        @{ ArchivePath = 'bin/x64/nvngx_dlss.license.txt'; OutputPath = 'bin/x64/nvngx_dlss.license.txt' }
        @{ ArchivePath = 'bin/x64/reflex.license.txt'; OutputPath = 'bin/x64/reflex.license.txt' }
    )
}
