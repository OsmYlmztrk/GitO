# Unreal asset merge: durum ve mimari

MyGitGUI'de çakışan Unreal asset'lerini (`.uasset`) değişiklik değişiklik birleştiren özelliğin özeti: ne yapıldı, hangi sınıf ne işe yarıyor, ne kaldı.

Son güncelleme: 2026-10-04 · Dal: `mygitgui` · İlgili commit'ler: `cbc76e4c`, `7922c0a6`, `c448c7c6`

---

## 1. Neden var?

Unreal'ın kendi merge penceresi bir asset'in yalnızca bir tarafını seçtirir (Accept Source / Accept Target). İki kişi aynı Blueprint'in farklı yerlerini değiştirdiğinde birinin işi kaybolur.

Bu özellik, git'in metin dosyalarında yaptığını asset'ler için yapar:
- Yalnızca bir tarafın değiştirdiği her şey otomatik alınır.
- İki tarafın aynı şeyi farklı değiştirdiği yerler kişiye sorulur.

## 2. Ne destekleniyor?

| Tür | Değişiklik değişiklik birleşen | Elle yapılması için listelenen ("unsupported") |
|---|---|---|
| Blueprint (`BP_`) | Node'lar, bağlantılar, pin değerleri, değişkenler, function graph ekleme / silme / yeniden adlandırma, class default'lar, var olan component'lerin ayarları | Event graph ve macro ekleme / silme, component ekleme / silme / taşıma, interface'ler, parent class, node'un başka graph'a taşınması |
| Data Asset (`DA_`) | Her property; struct alanları tek tek | Asset sınıfının değişmesi, instanced subobject'ler |
| Data Table (`DT_`) | Satır ekleme / silme, satırların her alanı | Row struct değişikliği |
| Material Instance (`MI_`) | Parametre override'ları, static switch'ler, parent, base property override'ları | Material layer'lar, static component mask'ler |

Ek kurallar:
- **Diziler, map'ler ve set'ler tek değer sayılır.** İki taraf aynı diziyi farklı değiştirirse çakışma çıkar.
- **Desteklenmeyen türler** (Material, `.umap`, Behavior Tree, Sound Cue, Niagara, Sequence, mesh, texture…) için plugin asset'i reddeder. Bu durumda GUI, Unreal'ın kendi merge penceresini açar.

## 3. Akış

```
MyGitGUI (Conflict paneli)                       Unreal (GitBlueprintMerge plugin)
───────────────────────────                      ──────────────────────────────────
"Merge in Unreal"
  │
  ├─ 1. Base / Ours / Theirs'i git'ten oku
  │     (CheckoutConflictStages, LFS dahil)
  │
  ├─ 2. Commandlet: arka planda, pencere yok ──► -run=GitBlueprintMerge
  │                                               snapshot → plan → (çakışma yoksa) apply + kaydet
  │     ◄── report.json ─────────────────────────
  │
  ├─ 3. Çakışma varsa: merge penceresi ────────► -GitBlueprintMergeUI
  │                                               kişi her çakışma için ours / theirs seçer → Merge
  │     ◄── report.json + Merged.uasset ─────────
  │
  ├─ 4. Sonucu çalışma kopyasına yaz ve stage'le
  └─ 5. Bildirim + görev çubuğu yanıp söner
```

Plugin, asset'i anlamıyorsa (rapor yazılmamışsa) GUI 3. adımda Unreal'ın kendi merge penceresine (`-diff`) düşer.

## 4. Plugin (`unreal/GitBlueprintMerge`, C++, UE 5.8, MIT)

### 4.1 Temel fikir: "fact"ler

Her revizyon düz bir anahtar → değer tablosuna (`FSnapshot::Facts`) çevrilir. Anahtarlar Unreal'ın kaydettiği kalıcı GUID'lerden ve isimlerden kurulur, böylece aynı öğe üç revizyonda eşleşir.

| Anahtar | Ne |
|---|---|
| `node/<guid>`, `node/<guid>/pos\|comment\|enabled` | Blueprint node'u ve ayrıntıları |
| `pin/<node>:<in\|out>:<pin>` | Pin'in yazılı değeri |
| `link/<pin>\|<pin>` | İki pin arasındaki bağlantı |
| `var/<guid>`, `cdo/<property>` | Değişken ve class default |
| `component/<guid>/prop/<name>` | Component ayarı |
| `graph/<guid>` | Graph (function, event, macro…) |
| `prop/<path>` | Data Asset / MI property'si (`A.B`, `A[1]`) |
| `row/<row>`, `row/<row>/<field>` | Data Table satırı ve alanı |
| `param/<dizi>/<parametre>`, `switch/<parametre>` | MI parametre override'ı, static switch |

Her fact için standart 3-way kuralı uygulanır:
- Yalnızca ours değiştirdiyse → ours alınır.
- Yalnızca theirs değiştirdiyse → theirs alınır.
- İkisi aynı değiştirdiyse → both.
- İkisi farklı değiştirdiyse → conflict.

Üstüne birlikte karar verilmesi gerekenler gruplanır. Örnekler:
- Bir tarafın sildiği node'u diğer tarafın düzenlemesi
- Tek bağlantı alan bir pin'e iki tarafın farklı bağlaması
- Silinen bir değişkeni diğer tarafın kullanması

Kişi her **çakışma grubu** için bir kez karar verir.

### 4.2 Dosyalar ve sınıflar

| Dosya | Sınıf / fonksiyon | Görevi |
|---|---|---|
| `BlueprintSnapshot.h/.cpp` | `FSnapshot`, `TakeSnapshot` | Blueprint'i fact tablosuna çevirir |
| | `PlanMerge`, `FMergePlan`, `FDecision` | 3-way karşılaştırma ve çakışma grupları |
| | `ResolveConflicts`, `TakeBoth`, `FindInsertions` | Seçimleri plana uygular. "Take both": iki tarafın aynı exec bağlantısına eklediği node'ları art arda koyar |
| | `IsUnsupported`, `FindUnsupported`, `LeaveOutUnsupported` | Henüz alınamayan değişiklikleri bulur, istenirse dışarıda bırakır |
| | `Describe`, `SummarizeGroup`, `DescribeSubject/Value` | Raporda ve pencerede görünen okunabilir metinler |
| `BlueprintMergeApply.h/.cpp` | `ApplyTheirs` | Planı ours'un bir kopyasına uygular. Node kopyalama, bağlantılar, değişkenler, function graph'lar ve component ayarlarını yapar, sonra derler |
| `AssetMerge.h/.cpp` | `EAssetKind`, `GetAssetKind` | Asset türünü belirler (Blueprint / DA / DT / MI / desteklenmiyor) |
| | `TakeDataSnapshot`, `TakeAssetSnapshot` | DA, DT ve MI için fact tablosu. `TakeAssetSnapshot` türe göre doğru snapshot'ı seçer |
| | `ApplyTheirsToData` | DA, DT ve MI için apply. Değerleri metin export/import ile kopyalar; MI'da `UpdateStaticPermutation` çağırır |
| | `FSnapshot::Unmergeable` | Karşılaştırılan ama henüz birleştirilmeyen fact'ler. Çakışmaya girerlerse "unsupported" olarak listelenir |
| `MergeSession.h/.cpp` | `FMergeSession` | Üç revizyonu yükler, planı tutar, önizleme üretir (`UpdatePreview`), merge'ü yapıp kaydeder, `report.json` yazar. Commandlet ile pencere bunu ortak kullanır |
| | `FRevision`, `LoadRevision`, `RenameRevision`, `SaveRevision` | Revizyonları `Saved/Diff` altına kopyalayıp yükler; sonucu gerçek paket adıyla kaydeder |
| `GitBlueprintMergeCommandlet.h/.cpp` | `UGitBlueprintMergeCommandlet` | `-run=GitBlueprintMerge`: pencere açmadan analiz eder; çakışma yoksa (veya `-Prefer` / `-Choices` verildiyse) birleştirir |
| `SGitBlueprintMergeWindow.h/.cpp` | `SGitBlueprintMergeWindow` | Unreal içindeki merge penceresi (Slate). İçeriği: çakışma kartları, otomatik birleşenler listesi, ours / base / result / theirs sütunları, 4 yerleşim. Blueprint'te graph, diğerlerinde değer paneli ve details paneli gösterir |
| `GitBlueprintMergeModule.cpp` | `FGitBlueprintMergeModule` | Editör `-GitBlueprintMergeUI` ile açılınca ana pencereyi gizler, merge penceresini açar, kapanınca raporu yazıp editörden çıkar |
| `GitBlueprintMergeTestCaseCommandlet.h/.cpp` | `UGitBlueprintMergeTestCaseCommandlet` | Test yardımcıları: `-DataFixtures` (DA/DT/MI), `-BlueprintFixture` (BP) ve `-Dump=<dosya>` (bir sonucun fact'lerini log'a döker) |
| `build-plugin.ps1` | — | Plugin'i ASCII bir temp klasörde derler, `%APPDATA%\MyGitGUI\UnrealPlugins\<sürüm>\` altına kurar |

### 4.3 Komut satırı

```
UnrealEditor-Cmd.exe <proje>.uproject -run=GitBlueprintMerge
    -PLUGIN=<...>\GitBlueprintMerge.uplugin -EnablePlugins=GitBlueprintMerge
    -Base=<dosya> -Ours=<dosya> -Theirs=<dosya> -Report=<json>
    [-Output=<dosya> -PackageName=/Game/... ] [-Prefer=ours|theirs | -Choices=<json>] [-LeaveOutUnsupported]

UnrealEditor.exe <proje>.uproject -GitBlueprintMergeUI ... (aynı parametreler, artı -OursLabel / -TheirsLabel)
```

Exit code güvenilmez, çünkü projedeki alakasız hataları da sayar. Doğru bilgi her zaman `report.json`'dadır.

## 5. MyGitGUI tarafı (C#, Avalonia)

| Dosya | Sınıf | Görevi |
|---|---|---|
| `src/Models/UnrealEngine.cs` | `UnrealEngine` | Asset'in `.uproject`'ini, editörü (launcher / registry / kaynak build) ve kurulu plugin'i bulur; dosya yolundan paket adı üretir (`Content/A/B.uasset` → `/Game/A/B`) |
| `src/Models/BlueprintMergeReport.cs` | `BlueprintMergeReport`, `…Summary`, `…Conflict`, `…Change` | Plugin'in `report.json`'unun C# karşılığı |
| `src/Commands/CheckoutConflictStages.cs` | `CheckoutConflictStages` | Çakışan dosyanın Base / Ours / Theirs halini temp dosyalara yazar (`git checkout-index --stage=all`, LFS çözülür) |
| `src/Commands/QueryConflictFileState.cs` | (ek) `IsConflictedLFSPointer` | İçinde çakışma işaretleri olan LFS pointer'ını "binary çakışma" olarak tanır |
| `src/ViewModels/UnrealMerge.cs` | `UnrealMerge` (Popup), `UnrealMergeStep`, `UnrealMergeTarget`, `UnrealMergeOutcome` | Adım adım ilerleyen merge popup'ı: oku → analiz → karar → stage. 3 modu var: `Auto`, `Resolve`, `UnrealWindow`. Süre gösterir, Terminate ile durdurulur |
| `src/ViewModels/Conflict.cs` | `Conflict` (Unreal kısmı) | Kartın durumunu tutar (plugin var mı, meşgul mü, son sonuç). "Compare / Merge / Resolve in Unreal" ve "Unreal's merge window" komutları burada |
| `src/Views/Conflict.axaml(.cs)` | — | Conflict panelindeki "Unreal Editor" kartı, ⓘ "What Merge in Unreal can combine" popup'ı, son çalıştırmanın sonuç listesi |
| `src/Views/UnrealMerge.axaml(.cs)` | — | Merge popup'ının görünümü |
| `src/App.axaml.cs` | `App.RequestAttention` | Uygulama arka plandayken iş bitince görev çubuğunu yakıp söndürür ve ses çalar |
| `src/App.JsonCodeGen.cs` | — | `BlueprintMergeReport` için JSON kaynak üretimi |

## 6. Test

Plugin'i editörü açmadan, MergeLab projesine karşı commandlet'lerle test ediyoruz:

```
-run=GitBlueprintMergeTestCase -DataFixtures -OutDir=<klasör>      DA/DT/MI_Fixture_Base/Ours/Theirs
-run=GitBlueprintMergeTestCase -BlueprintFixture -OutDir=<klasör>  BP_Fixture_Base/Ours/Theirs
-run=GitBlueprintMerge -Base= -Ours= -Theirs= -Report= -Output= -Prefer=theirs
-run=GitBlueprintMergeTestCase -Dump=<Merged.uasset>              sonucun fact'leri
```

MergeLab'de hazır bir çakışma da bekliyor: `bp/ours` dalı üzerinde `bp/theirs` merge edildi, 4 dosya çakışmalı.

| Dosya | Çakışmalar | Otomatik birleşenler |
|---|---|---|
| `BP_Fixture` | BeginPlay mesajı, `Health` default'u, `Light` intensity | 16 değişiklik (yeni node, `ResetStats` fonksiyonu, `Armor` / `Ammo`, `Speed`, `InitialLifeSpan`, ışık rengi, mesh gölgesi…) |
| `DA_Fixture` | `LoadFilter` | `DebugColor.R` / `G`, `bSupportsActorFilters` |
| `DT_Fixture` | C satırı (ours sildi, theirs düzenledi) | A / B alanları, D ve E satırları |
| `MI_Fixture` | `Roughness` | `Metallic`, `Tint`, `UseDetail` switch'i, `TwoSided` |

Dallar:
- `bp/base` → `bp/ours`, `bp/theirs`: bu senaryo.
- `data/base` → `data/ours`, `data/theirs`: yalnızca DA, DT ve MI.
- `door/*`: eski BP_Door senaryosu.

Merge penceresinin görsel testi elle yapılıyor (GUI otomasyonu kullanmıyoruz).

## 7. Ne kaldı?

### Bilinen riskler
- **WBP / ABP sessiz kayıp.** Widget Blueprint'in widget ağacı ve animasyonları, Anim Blueprint'in AnimGraph'ı snapshot'a girmiyor. Theirs oralarda bir şey değiştirdiyse uyarısız kaybolur. En azından `Unmergeable` olarak raporlanmalı. Bu iş ayrı görev olarak önerildi.

### Birleştirme kapsamı
- Blueprint: event graph ve macro ekleme / silme, component ekleme / silme / taşıma, interface, parent class, node'u başka graph'a taşıma.
- Diziler, map'ler ve set'ler eleman bazında birleşmiyor. Parametre dizileri (MI) dışında anahtar yok.
- Instanced subobject içerikleri, material layer'lar ve static component mask'ler birleşmiyor.
- Data Table'da theirs'in eklediği satırlar sona ekleniyor; satır sırası korunmuyor.
- Yeni türler, kolaydan zora: String Table, Curve Table, User Defined Enum / Struct, Blackboard, Behavior Tree, Material, Level.

### Dağıtım ve altyapı
- Plugin yalnızca UE 5.8 için derleniyor. Başka sürüm için `build-plugin.ps1 -EngineVersion X` gerekiyor; bu da makinede Visual Studio ister. Hazır binary dağıtımı yok.
- Plugin'in adı hâlâ "GitBlueprintMerge", ama artık başka türleri de birleştiriyor. İstenirse yeniden adlandırılabilir.
- `report.json`'daki `kind` alanını GUI henüz kullanmıyor.
- Otomatik test / CI yok; fixture'lar elle çalıştırılıyor.

### Bekleyen küçük işler
- `.mcp.json` (yerel Unreal MCP ayarı) commit edilmedi.
- Değişiklikler push edilmedi.
- `Unreal Projects\MergeLabData` artık gereksiz bir test kopyası; silinebilir.
