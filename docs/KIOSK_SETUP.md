# Jukebox hesabını kurma

Windows 10 veya 11'in 64 bit Pro, Enterprise ya da Education sürümü gerekir.
Kurulum için yönetici onayı gerekir. Uygulama standart kullanıcı yetkisiyle çalışır.

1. Uygulama ZIP dosyasını yerel bir klasöre çıkarın.
2. `Install-Jukebox.ps1`, `neon_jukebox.exe` ve `assets` klasörü yan yana bulunsun.
3. Bu klasörde PowerShell açıp aşağıdaki komutu çalıştırın. Windows sorarsa **Evet** seçin.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-Jukebox.ps1
```

Komuttaki yürütme ilkesi yalnızca bu PowerShell işlemi için geçerlidir; bilgisayarın
kalıcı betik ayarını değiştirmez. Betik internete bağlanmaz ve ek araç indirmez.

Kurulum tamamlanınca **Ctrl+Alt+Delete → Kullanıcı değiştir → Jukebox** yoluyla
giriş yapın. Windows hesabının parolası yoktur. Uygulama masaüstü yerine açılır;
uygulama kapanırsa başlatıcı birkaç saniye sonra yeniden açar. İlk kullanımda
uygulamanın yönetici PIN'ini ve müzik/video klasörlerini belirleyin. Bu PIN,
Windows hesabının parolasından ayrıdır.

Uygulama `%ProgramData%\NeonJukebox` içine kopyalanır. Uygulama dosyaları normal
kullanıcılar tarafından değiştirilemez; Jukebox hesabı kendi ayarlarını ve
`library` önbelleğini yazabilir. Kurulum günlüğü
`%ProgramData%\NeonJukebox.kiosk-setup.json` dosyasındadır. Başlatıcı günlüğü
Jukebox profilindeki `%LOCALAPPDATA%\NeonJukebox\shell.log` dosyasındadır.

Diğer hesapların masaüstü ayarı korunur. Windows'a otomatik giriş etkinleştirilmez;
açılışta Jukebox hesabını kullanıcı seçer. Windows açılış logosu ve oturum ekranları
görünebilir. Bu düzen, tam bir erişim kilidi değildir: Ctrl+Alt+Delete ve diğer
sistem araçlarına erişim ayrıca kısıtlanmaz.

Ağdaki müzik/video klasörleri için Jukebox hesabına da erişim tanımlanmalıdır.
Başka bir hesaptaki Z: gibi sürücü eşlemeleri ve kayıtlı ağ parolaları otomatik
aktarılmaz. Kaynaklarda doğrudan `\\sunucu\paylasim` yolları kullanılabilir.
Betik herhangi bir kişisel PIN, medya yolu veya ağ parolası içermez.

## Güncelleme ve yeniden deneme

Jukebox hesabından **oturumu kapatın**, kendi yönetici hesabınıza geçin ve yeni
uygulama paketindeki betiği yeniden çalıştırın. Aynı hesap ve kullanıcı ayarları
korunur. Yarım kalan kurulum da aynı betikle sürdürülebilir. Başka amaçla açılmış,
aynı adlı bir hesap betik tarafından sahiplenilmez veya değiştirilmez.

## Müzik ve video kaynaklarını sıfırlama

Jukebox hesabından oturumu kapatıp yönetici hesabınızda çalıştırın:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-Jukebox.ps1 -ResetSources
```

Bu seçenek yalnızca Jukebox hesabının müzik/video kaynaklarını, kataloğunu,
kuyruğunu ve kayıtlı çalma durumunu temizler. Önce dosyaları kullanıcı profilindeki
`backups\sources-...` klasörüne yedekler. Uygulama PIN'i, tema, ses seviyesi ve
asıl müzik/video dosyaları korunur. Sonraki girişte kaynak seçimi yeniden yapılır.

## Normal masaüstüne dönme

Jukebox hesabından oturumu kapattıktan sonra yönetici hesabınızda çalıştırın:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-Jukebox.ps1 -RestoreDesktop
```

Jukebox hesabında normal masaüstü açılır. Hesap ve uygulama dosyaları silinmez.

Kurulum planını değişiklik yapmadan görmek için `-WhatIf`; uygulama başka bir
klasördeyse `-AppDirectory 'D:\Neon Jukebox'` kullanılabilir.
