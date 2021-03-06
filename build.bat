rem build file for virtualbox
rem How do we make portable?

rem run the VC thing
"\Program Files (x86)\Microsoft Visual Studio 10.0\VC\vcvarsall.bat"

rem run the config
cscript configure.vbs  --with-mingw-w64=\sandbox\work\mingw64 --with-openssl=\sandbox\work\OpenSSL --with-libcurl=\sandbox\work\curl\lib --with-VC="\Program Files (x86)\Microsoft Visual Studio 10.0\VC" --with-python=\sandbox\Python27

rem opinion is divided whether you need this
env

rem you definitely need this
kBuild\envwin.cmd --win64

rem Other things we might need
rem
rem   do we need nm? (from mingw64/bin)


rem Need to copy in the signing stuff
mkdir tools
mkdir tools\win.x86
mkdir tools\win.x86\selfsign
mkdir tools\win.x86\selfsign\r1

cp "\Program Files (x86)\Microsoft SDKs\Windows\v7.0A\Bin\signtool.exe" tools\win.x86\selfsign\r1

rem find the tools
set PATH=%PATH%;\sandbox\work\repos\github\virtualbox\tools\win.amd64\bin

rem and Python ffs
set PATH=%PATH%;\sandbox\Python

rem run the build
kmk

rem end
