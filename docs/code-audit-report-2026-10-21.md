# MiniLang IDE 浠ｇ爜瀹¤鎶ュ憡锛?026-10-21锛?
> 瀹¤杞锛?026-10-21 | 鏂规硶锛? 涓苟琛屽彧璇诲璁″瓙浠ｇ悊锛堝崗绋嬬湡鎸傝捣 / JIT 鍧楃紦瀛?/ 娉涘瀷鎿﹂櫎鍏偣 / 瀛楀吀閫忔槑鏌ユ壘涓?VM 鐑矾寰勶級+ 涓诲璁″憳閫愰」澶嶆牳涓庢帰閽堝疄娴?> 鍩虹嚎锛歷1.4.0锛坋937640锛? 鍦ㄩ€旀壒娆★紙鍗忕▼ Linux ucontext / 娉涘瀷鎿﹂櫎琛ュ叏 + 鏄惧紡绫诲瀷瀹炲弬锛岀幇宸插叆搴?c132756銆乤2aeefd锛?> 鏈疆淇锛? 椤癸紙P0脳1銆丳1脳3銆丳2脳1锛夛紝鎻愪氦 d4420d8锛涢獙璇?= Windows debug 鍏ㄩ噺 ctest **4156/4156 閫氳繃**锛?W4+/WX 闆惰鍛婏紝鍚?MINILANG_BUILD_PERF_TESTS=ON 鍏ㄥ彛寰勶級
> 杈撳嚭鏍煎紡锛氭寜 AGENTS 搂5锛堜綅缃?/ 涓ラ噸鎬?/ 绫诲瀷 / 澶嶇幇鍦烘櫙 / 鏍瑰洜鍒嗘瀽 / 淇寤鸿锛夛紱**淇寤鸿浠呴拡瀵规湭淇椤?*锛堝凡淇椤硅 搂2锛?
## 1. 瀹¤鑼冨洿

| 瀹¤鍩?| 鑼冨洿 | 缁撹姒傝 |
|---|---|---|
| 鍗忕▼鐪熸寕璧?| InterpreterCoroutine.cpp锛圵indows fiber + Linux ucontext锛夈€丆oroutineFiber.h銆両nterpreter.cpp callCoroutineNext/鏍归泦鏀堕泦銆丟cManager 鍗忕▼ mark 鍒嗘敮 | P0脳1銆丳1脳1銆丳2脳3锛沄M 渚у揩鐓?閲嶅畾鍩烘湰杞湭瑕嗙洊锛堣 搂5锛?|
| JIT per-chunk 鍧楃紦瀛?| compiler/jit/锛圔lockCache銆乼iering 闂ㄦ帶銆両C銆丣itContext/JitFrame 鍗忚锛? jitBuildEnumVariant 杞借嵎鏍￠獙 | P1脳1銆丳2脳1銆丳3脳1銆佽褰暶?锛涢敭瑕嗙洊/瀵煎叆妲介噸缁戝畾/椹卞姩閫愬嚭鏃犵幇鏃跺彲杈剧己闄?|
| 娉涘瀷鎿﹂櫎鍏偣 + 鏄惧紡绫诲瀷瀹炲弬 | TypeChecker.h銆両nterpreter checkType/visitEnumVariantExpr銆丆ompiler/AstIRBuilder emitTypeCheck銆丼tackVM/RegisterVM enum 杞借嵎鏍￠獙銆丳arser trySkipExplicitTypeArgs | 鍏偣鎺ュ叆纭鏃犻仐婕忥紱P1脳1锛堟瀯閫犺矾寰勬椂搴忥紝宸蹭慨锛夈€丳2脳3銆丳2 娓呯悊椤姑?锛汮IT 绗竷鐐圭Щ浜ゅ悗纭锛堝凡淇級 |
| 瀛楀吀閫忔槑鏌ユ壘 + VM 鐑矾寰?| Value.h 閫忔槑鏌ユ壘鍩虹璁炬柦鍙?19 澶勬帴鍏ョ偣銆乿alidateChunkInstructionBoundaries銆丟cManager slots_銆丷efCounted 鍐呭瓨搴?| **鏃?P0/P1**锛汸2脳2銆丳3脳2銆佹敞璁懊? |

澶嶆牳澹版槑锛氭墍鏈?P0/P1 鍙戠幇宸茬敱涓诲璁″憳璇绘簮鐮佺嫭绔嬫牳瀹烇紙闈炶浆杩板瓙浠ｇ悊缁撹锛夛紱璇箟琛屼负绫诲彂鐜扮粡 CLI/娴嬭瘯浜岃繘鍒舵帰閽堝疄娴嬶紙瑙?搂3.1銆伮?.2锛夈€?
## 2. 鏈疆宸蹭慨澶嶏紙鎻愪氦 d4420d8锛屾祴璇?+5锛屽叏閲?4156/4156 缁匡級

### 2.1 P0锝滄寕璧峰崗绋嬪揩鐓т笉鍦ㄤ换浣?GC 鏍归泦锛堜笁鎵ц鍚庣锛夛綔鏁版嵁鎹熷潖

- **浣嶇疆**锛歚interpreter/Interpreter.cpp` triggerIncrementalGc锛堢害 1251鈥?319 琛岋紝鏍归泦鏋氫妇 0鈥? 椤规棤鍗忕▼蹇収锛夛紱`interpreter/Value.h` gcRootPtr锛堝 VAL_COROUTINE 杩斿洖 nullptr锛夛紱`interpreter/InterpreterCoroutine.cpp` CoroutineFiberState锛沗GcManager.cpp` mark 涓や釜 VAL_COROUTINE 鍒嗘敮
- **绫诲瀷**锛欸C 璇洖鏀讹紙鏁版嵁鎹熷潖锛?- **澶嶇幇鍦烘櫙**锛歚fun* gen() { var a = [10,20,30]; yield a[0]; yield a[1]; } var g = gen(); g.next();`锛堟寕璧凤紝a 浠呰鐢熸垚鍣ㄥ眬閮ㄧ幆澧冩寔鏈夛級鈫?涓昏剼鏈户缁垎閰嶈法杩?GC 闃堝€?8192锛堣鍙ヨ竟鐣屽閲?GC锛夆啋 `g.next()` 鏃?a 鐨勫厓绱犲凡琚?sweep 娓呯┖锛岀储寮曡秺鐣?- **鏍瑰洜**锛氭寕璧锋湡闂村抚宸茬Щ鍑?callStack_銆佺幆澧冮摼浠呭瓨浜?`CoroutineFiberState::suspendedEnv/funEnv/suspendedCallStack`锛坰hared_ptr 鍙繚瀵硅薄瀛樻椿銆佷笉鏋勬垚 mark-sweep 鏍癸級锛沗gcRootPtr()` 瀵瑰崗绋嬭繑鍥?nullptr 浣跨幆澧冨彉閲忕洿鎺ユ寔鏈夌殑鍗忕▼鏁翠綋涓嶈繘鏍归泦锛沗CoroutineFiberState` 娉ㄩ噴澹扮О"env/甯ф寔 GC 鏍?浣嗕粠鏈帴绾库€斺€攙1.4.0 鏂囨。涓庡疄鐜版柇瑁傘€俈M 渚у悓鐞嗭紙鎸傝捣绐楀彛鍐?VM 鏍堝凡娓呯┖锛屽揩鐓?stackSuffix/瀵勫瓨鍣ㄧ獥鍙ｄ笉鍙锛沄M 鎵ц鏈熻櫧鏈?CallbackSuppressor 鎶戝埗澧為噺 GC锛屼絾鎸傝捣绐楀彛涓嶅彈淇濇姢锛?- **淇**锛氣憼 `gcRootPtr()` 琛ュ崗绋嬪垎鏀紙瀵归綈 BUG-REPL-AUDIT-7 闂寘鍏堜緥锛欳oroutineData 涓嶇櫥璁?slots_ 涓嶄細琚?sweep锛屼綔涓烘牴璁?mark 杩涘叆鍗忕▼鍒嗘敮锛夛紱鈶?`CoroutineData::gcRootContributor` 绫诲瀷鎿﹂櫎鍥炶皟鈥斺€擨nterpreter 璐＄尞蹇収鐜閾撅紙collectCoroutineFiberGcRoots锛夈€丼tackVM 璐＄尞 stackSuffix + 甯у唴宸插叧闂?upvalue銆丷egisterVM 璐＄尞瀵勫瓨鍣ㄧ獥鍙?+ 宸插叧闂?upvalue锛涒憿 GcManager mark 涓や釜 VAL_COROUTINE 鍒嗘敮鎺ュ叆锛堟牴闆嗗惊鐜?switch 鎶藉彇涓?`markTrackedRoot` 鍏辩敤锛屾秷闄ゅ弻浠?switch 婕傜Щ椋庨櫓锛夛紱鈶?涓㈠純蹇収澶勫悓姝ョ疆绌哄洖璋冿紙闃叉偓鍨傦級
- **鍥炲綊**锛歚CoroutineSuspension.SuspendedGeneratorLocalsSurviveIncrementalGc`锛? 涓囨鍒嗛厤纭繚澶氭璺ㄩ槇鍊硷級

### 2.2 P1锝滄硾鍨嬫瀯閫犺矾寰勫弬鏁?榛樿鍊兼牎楠屽厛浜庣被鍨嬪弬鏁板悎骞?guard锝滃洓鍚庣涓嶄竴鑷?
- **浣嶇疆**锛歚interpreter/InterpreterCalls.cpp` setupInitEnvironment锛坈heckType 浜庡弬鏁?榛樿鍊肩粦瀹氬惊鐜級+ runInitMethodBody锛坢ergedTypeParams + CallFrameGuard 鍦ㄥ叾鍚庢墠寤虹珛锛夛紱鍚屽瀷 `interpreter/Interpreter.cpp` visitVarDecl 鑷姩鏋勯€犺矾寰勶紙榛樿鍊?checkType 鍏堜簬 guard锛?- **绫诲瀷**锛氳涔変笉涓€鑷达紙杩濆弽 ADR-002锛?- **澶嶇幇鍦烘櫙**锛歚class Box<T> { init(v: T[]) { } } var b = Box([1, 2]);` 鈫?淇鍓?Interpreter 鎶?绫诲瀷娉ㄨВ杩濆弽: 鏈熸湜绫诲瀷 T[]"锛孲tackVM/RegisterVM/JIT 閫氳繃锛圴M 鏃犲弬鏁版牎楠屽彂灏勭偣锛夛紱涓斾笌 init 浣撳唴鍚屾敞瑙ｈ鎿﹂櫎鑷浉鐭涚浘
- **鏍瑰洜**锛歝heckType 鐨勬摝闄や緷璧?`currentTypeParams_`锛屾瀯閫犺矾寰勭殑鍙傛暟鏍￠獙鍙戠敓鍦ㄥ悎骞?guard 寤虹珛涔嬪墠锛堟鍒讳负璋冪敤鏂逛笂涓嬫枃锛岄《灞備负绌猴級
- **淇**锛氬悎骞?guard 鍓嶇Щ鑷充袱澶勫弬鏁板惊鐜箣鍓嶏紙CallFrameGuard{manageCtx=false} 浠呯鐞嗙被鍨嬪弬鏁拌澧冿級锛涘榻?invokeMethod/callNamedFunction 鐨?鍏堝悎骞跺悗鏍￠獙"椤哄簭
- **鍥炲綊**锛歚GenericErasureCompletion.GenericClassInitParamCompositeAnnotation`锛堝洓鍚庣锛夈€乣GenericClassInitDefaultTParamAnnotation`锛堟爣閲?T 榛樿鍊硷紝缁忔帰閽堢‘璁よ涔夊悗鏀瑰舰锛?
### 2.3 P1锝淛IT `jitBuildEnumVariant` 杞借嵎鏍￠獙鏈崌绾ч€掑綊鎿﹂櫎锛堢涓冨鎿﹂櫎鐐癸級锝滃洓鍚庣涓嶄竴鑷?
- **浣嶇疆**锛歚compiler/jit/JITRuntime.cpp:1013`锛坖itBuildEnumVariant 鍐咃級
- **绫诲瀷**锛氳涔変笉涓€鑷?- **澶嶇幇鍦烘櫙**锛歚enum Row<T> { Cell(T[]) } var r = Row.Cell([1, 2]); print(r);` 鈫?淇鍓?JIT 鎶?绫诲瀷涓嶅尮閰?涓锛孖nterpreter/StackVM/RegisterVM锛堝惈 IR 璺緞锛夋瀯閫犳垚鍔熴€傝Е鍙戞潯浠?= 娉涘瀷 enum 鍙樹綋 + 澶嶅悎娉ㄨВ杞借嵎 + 闈炵┖鏁扮粍锛堢┖鏁扮粍 vacuously 閫氳繃锛涜８ T 涓嶈Е鍙戯級
- **鏍瑰洜**锛歄P_BUILD_ENUM_VARIANT 鏄?JIT **鍘熺敓 helper 璺緞**锛圝ITCodeGen.cpp:1214 鈫?emitBuildEnumVariant锛屾棤鍥為€€ VM 閫氶亾锛夛紝R163 鍏偣淇娓呭崟涓嶅惈 JIT锛沗expectAllGenericBackends` 涓嶅惈 JIT 閫氶亾锛屽洖褰掔綉鏈鐩?- **淇**锛氬崌绾т负 `annotationReferencesTypeParam`锛堜笌鍏鍚屽垽瀹氾級锛涜ˉ JIT 閫氶亾鍥炲綊 `EnumPayloadCompositeTAnnotationJIT`
- **绾緥**锛氭硾鍨嬫敞瑙ｆ摝闄ょ偣鐜颁负**涓冨**锛堝師鍏 + JIT锛夆€斺€擜GENTS 搂3 #11 宸叉寜涓冨鍙ｅ緞缁存姢

### 2.4 P1锝滃け璐ュ崗绋嬪純鐢ㄩ噸寤哄鑷?yield 璁℃暟閿欎綅锝滆涔夊垎姝э紙fiber vs 閲嶆斁锛?
- **浣嶇疆**锛歚interpreter/InterpreterCoroutine.cpp` callCoroutineNext failed 鍒嗘敮 + coroutineSuspend锛坈urrentYieldId 鏃犳潯浠惰嚜澧烇級
- **绫诲瀷**锛氳涔変笉涓€鑷?/ 閿欒浼犳挱
- **澶嶇幇鍦烘櫙**锛堟帰閽堝疄娴嬶級锛歚fun boom() { throw "boom"; } fun* g() { yield 1; boom(); yield 2; }` 渚濇 `.next()`锛氫慨澶嶅墠 fiber 璺緞绗笁娆?next() 浠庡ご閲嶈窇鍑芥暟浣?鈫?杩斿洖棣栦釜 yield 鍊间笖鎻愬墠缃?done锛涢噸鏀捐矾寰勫悓鐐瑰啀鎶?- **鏍瑰洜**锛氶敊璇€冮€稿悗 reset 閲嶅缓 + coroutineSuspend 鏃犳潯浠堕€掑锛屼笌閲嶆斁"璺宠繃宸叉秷璐?yield 鍚庡悓鐐瑰啀鎶?鍒嗘
- **淇**锛氬け璐?fiber 淇濈暀锛宍fs->failed` **鍓嶇疆妫€鏌?*锛堢粷涓?switch 杩涘凡缁堢粨 fiber 鏍堚€斺€斿師瀹炵幇浠呭湪 switch 杩斿洖鍚庢鏌ワ紝淇濈暀 fiber 鍚庡墠缃鏌ョ己澶卞嵆閲嶅叆姝绘爤 UB锛屾帰閽堝疄娴?0xC0000005 宕╂簝鍚庝慨姝ｏ級+ 杩斿洖鐐瑰弻妫€鏌ワ紱鍚庣画 .next() 鎸佺画閲嶆姏鍚屼竴閿欒锛堝€?閿欒搴忓垪涓庨噸鏀鹃€愮偣涓€鑷达紝鍓嶇紑鍓綔鐢ㄥ彧鎵ц涓€娆♀€斺€擜DR-007 鏃㈠畾宸紓鍙ｅ緞锛?- **鍥炲綊**锛歚CoroutineSuspension.FailedGeneratorRethrowsSameErrorOnEveryNext`锛堣В閲婂櫒涓夋搴忓垪锛? `FailedGeneratorErrorEscapePrefix`锛堝洓鍚庣涓€鑷村墠缂€锛?- **娴嬭瘯璁捐娉ㄨ**锛歵hrow 蹇呴』浣嶄簬闈炴渶缁?yield 涔嬪悗锛堢粡杈呭姪鍑芥暟閫冮€革級鈥斺€旂敓鎴愬櫒鏈€缁?yield 涔嬪悗鐨勫熬閮ㄨ鍙ヤ笉鎵ц鏄棦鏈夎璁★紝鐩存帴鍐欏湪鏈熬鏄浠ｇ爜锛堟帰閽?coverage 瀹炴祴琛?MISS锛?
### 2.5 P2锝淛IT 鍧楃紦瀛?storeBlocksToCache 鏈炕杞疄渚嬩晶 owned 鏍囧織锝滄墍鏈夋潈鍗忚杩濆弽

- **浣嶇疆**锛歚compiler/jit/JIT.cpp` storeBlocksToCache锛堢害 810鈥?52 琛岋級
- **绫诲瀷**锛歎AF 娓╁簥锛堢幇鏃惰 asmjit 瀹归敊鎺╃洊锛?- **澶嶇幇鍦烘櫙**锛歟ager 鍙紦瀛樼紪璇?鈫?鍧楀叆缂撳瓨 鈫?鍚屽疄渚嬪啀娆?execute() 鍏ュ彛/鏋愭瀯 鈫?瀹炰緥 `runtime_.release` 閲婃斁鍏变韩 runtime 鐨勫潡鈥斺€旇法 JitAllocator 閲婃斁锛坅smjit 鎸夋煡鍧楀け璐?no-op锛況untime 鍚堜竴鍗冲弻閲嶉噴鏀?UAF锛?- **鏍瑰洜**锛氭敞閲婁笌鍛戒腑璺緞涓嶅彉閲忓潎澹版槑"缂撳瓨鍏变韩鍧楀疄渚嬩笉閲婃斁锛坥wned=false锛?锛屼絾 miss鈫抯tore 璺緞婕忕炕杞?- **淇**锛歴tore 閫愬潡鎷疯礉鍚庣疆 `blk.owned = false`

## 3. 鏈慨澶嶅彂鐜帮紙鎸変弗閲嶆€ф帓搴忥紱鍧囦负鏈疆鏂扮櫥璁帮紝淇寤鸿鍚湪鏉＄洰鍐咃級

### 3.1 P1锝淰M 閿欒閫冮€稿悗 `.next()` 璇箟涓庤В閲婂櫒鍒嗘锛堝洓鍚庣涓嶄竴鑷达紝棰勫瓨锛?
- **浣嶇疆**锛歚compiler/backend-stack/VMCalls.cpp` callCoroutineNext 寮傚父绌块€忚矾寰勶紙绾?2181鈥?189 琛?涓嶇疆 done"锛変笌鍏跺悗鐨勬柊鐢熸垚鍣ㄨ皟鐢ㄥ垎鏀紱RegisterVMCalls.cpp 瀵瑰簲鐐?- **绫诲瀷**锛氳涔変笉涓€鑷?/ 閿欒浼犳挱
- **澶嶇幇鍦烘櫙**锛堟帰閽堝疄娴嬶紝鍚岃剼鏈洓鍚庣宸垎锛夛細`fun boom() { throw "b"; } fun* g() { yield 1; boom(); yield 2; }` 閿欒绌块€忓悗锛欼nterpreter锛坒iber 淇鍚庯級绗笁娆?`.next()` **鍚岀偣閲嶆姏**锛坉one 淇濇寔 false锛夛紱StackVM 绗笁娆?`.next()` **杩斿洖鏈?yield 鍊?1 涓旂疆 done=true**锛堝疄娴嬪簭鍒?`1E1false1true` vs 瑙ｉ噴鍣?`1E1false<runtime:鏈崟鑾风殑寮傚父: b>`锛?- **鏍瑰洜**锛歏M 閿欒绌块€忚矾寰勬湰韬笉缃?done锛屼絾寮傚父鍚?vmSuspension 宸叉秷璐广€佷笅娆?.next() 璧板叏鏂拌皟鐢ㄩ噸鏀锯€斺€斿疄娴嬫湭鎸夐鏈熷啀娆℃姏鍑鸿€屾槸杩斿洖鏈€肩疆 done锛屾帹鏂紓甯稿悗 `currentCoroutineTargetYieldId_/currentYieldExecutionCount_` 鐘舵€佷娇閲嶆斁闂ㄦ帶鐭矾锛堢‘鍒囨満鍒堕渶 VM 渚у崗绋嬪璁¤ˉ榻愶紝瑙?搂5锛?- **淇寤鸿**锛歏M 渚у崗绋嬪璁★紙鏈疆鏈鐩栵級纭鍚庣粺涓€鍙ｅ緞鈥斺€旀帹鑽愬榻愯В閲婂櫒"閲嶆姏鐩磋嚦鑰楀敖"锛堟敼 VM 鏂扮敓鎴愬垎鏀殑閿欒閲嶆斁闂ㄦ帶锛夛紝鎴栧叏鍚庣缁熶竴"閿欒鍗崇粓缁?锛堥渶鍚屾鏀硅В閲婂櫒 fiber + 閲嶆斁 + ADR-007 璇箟琛級锛涘睘璇箟鍙樻洿锛岄渶 ADR 绔嬮」
- **缃俊**锛氳涓哄樊寮備负鎺㈤拡瀹炴祴锛堢‘瀹氾級锛涙牴鍥犲畾浣嶄负涓?
### 3.2 P2锝滄暟缁勫瓧闈㈤噺榛樿鍙傛暟鍊奸潤榛樹笉缁戝畾锛堥瀛橈級

- **浣嶇疆**锛歚parser/Parser.cpp` parseParamList 榛樿鍊煎鐞嗭紙BUG-DEF-1 isLiteralDefaultExpr 鐧藉悕鍗曪級+ 涓夊悗绔粯璁ゅ€肩粦瀹?- **绫诲瀷**锛氶敊璇紶鎾?/ 涓夊悗绔竴鑷存€ц竟鐣?- **澶嶇幇鍦烘櫙**锛堟帰閽堝疄娴嬶級锛歚fun f(a: int[] = [1, 2]) { return a; } print(f());` 鈫?**杩愯鏃堕敊璇?鏈畾涔夌殑鍙橀噺: a"**锛堥粯璁ゅ€兼湭琚粦瀹氾級锛涚被 init 鍚屽瀷锛坄init(v: int[] = [1, 2])` 鈫?瀛楁 null锛屾棤浠讳綍璇婃柇锛?- **鏍瑰洜**锛欱UG-DEF-1 绛栫暐涓?榛樿鍊间粎瀛楅潰閲忥紝Parser 灞傛嫆缁濆鏉傝〃杈惧紡"锛屼絾鏁扮粍瀛楅潰閲忔湭瑙﹀彂 ParseError 鑰屾槸闈欓粯閫氳繃瑙ｆ瀽銆佺粦瀹氶樁娈典涪寮冿紙鐧藉悕鍗曚笌瑙ｆ瀽琛屼负涓嶄竴鑷达級锛汣LI 渚у琛屽舰寮忓彟鎶?2 涓?parse 閿欒锛屽崟琛屼笌澶氳琛屼负涓嶄竴鑷?- **淇寤鸿**锛歱arseParamList 瀵?ArrayLiteral/DictLiteral 榛樿鍊兼樉寮?ParseError锛堢瓥鐣ュ唴鑷唇锛夛紝鎴栨墿灞?BUG-DEF-1 鏀寔鏁扮粍/瀛楀吀瀛楅潰閲忛粯璁ゅ€硷紙VM 渚?0xFFFF 鍝ㄥ叺鏈哄埗宸叉湁鍏堜緥鈥斺€擨nterpolatedString锛夛紱鍚屾椂鏍告煡 CLI 涓庢祴璇曠绾胯В鏋愯涓轰笉涓€鑷?- **缃俊**锛氳涓哄疄娴嬬‘瀹氾紱鏈哄埗瀹氫綅涓?
### 3.3 P2锝滄硾鍨嬪祵濂楀嚱鏁?lambda/灞€閮?enum 涓嶇户鎵垮灞傜被鍨嬪弬鏁帮紙棰勫瓨锛屽洓鍚庣涓€鑷存嫆缁濆悎娉曠▼搴忥級

- **浣嶇疆**锛歚compiler/core/CompilerStmt.cpp:608`锛坈urrentTypeParams_ 瑕嗙洊寮忚祴鍊硷級銆乣compiler/ir/AstIRBuilder.cpp:2086`銆乣interpreter/InterpreterCalls.cpp` CallFrameGuard 涓夊锛?45/459/1035锛?- **绫诲瀷**锛氳竟鐣屾潯浠讹紙闈炰竴鑷存€х己闄封€斺€斿洓鍚庣鍚岄敊锛?- **澶嶇幇鍦烘櫙**锛歚fun f<T>(x: T) { fun inner(y) { var z: T[] = [y]; return z; } return inner(x); } print(f(1));` 鈫?鍥涘悗绔竴鑷存姤"绫诲瀷娉ㄨВ T[] 涓嶅尮閰?
- **鏍瑰洜**锛氱被鍨嬪弬鏁颁笂涓嬫枃鎸夊嚱鏁板０鏄庤鐩栬€岄潪璇嶆硶浣滅敤鍩熶紶鎾紱绫绘柟娉曡矾寰勫凡姝ｇ‘鍚堝苟锛圕ompilerClass.cpp:162-164 绛夛級
- **淇寤鸿**锛氳繘鍏ュ瓙鍑芥暟鏃?`currentTypeParams_ = 澶栧眰 鈭?node.typeParams`锛堢紪璇戞湡涓庤繍琛屾椂 CallFrameGuard 鍚屾锛夛紱灞€閮?enum 寮曠敤澶栧眰 T 闇€骞跺叆 EnumInfo.typeParams銆傚睘鑳藉姏缂哄彛鑰岄潪鍥炲綊锛屽缓璁珛椤?- **缃俊**锛氶珮锛堜笁澶勮祴鍊肩偣鐩存帴浠ｇ爜璇佹嵁锛涙湭杩愯楠岃瘉锛?
### 3.4 P2锝渢rySkipExplicitTypeArgs 鐧藉悕鍗曠己 `:`/`(`/`)`锛堥瀛橈級

- **浣嶇疆**锛歚parser/Parser.cpp:424-470`锛堢櫧鍚嶅崟鍒嗘敮 447-451 琛岋級
- **绫诲瀷**锛氳竟鐣屾潯浠?- **澶嶇幇鍦烘櫙**锛歚fun f<T>(x: T) { return x; } print(f<dict[string:int]>({ "a": 1 }));` 鈫?澶嶅悎绫诲瀷瀹炲弬鍥為€€涓烘瘮杈冭В鏋愶紝鎶ヨ瀵兼€ч敊璇?鏈畾涔夌殑鍙橀噺: dict"
- **鏍瑰洜**锛氭帰娴嬬櫧鍚嶅崟鏈鐩?parseTypeAnnotation 鍙敓鎴愮殑 dict[..]/fun(..) 褰㈡€佹墍闇€鐨?`:`/`(` token
- **淇寤鸿**锛氱櫧鍚嶅崟鍔犲叆 TK_COLON/TK_LPAREN/TK_RPAREN 骞堕厤瀵瑰钩琛¤鏁帮紙瑁稿姞浼氳鍚?`x < (a+b) > (c)`锛夛紝鎴栫洿鎺ュ鐢?parseTypeAnnotation 鐨勫钩琛℃秷鑰楋紱鍥為€€闆舵畫鐣欐満鍒舵湰韬凡楠岃瘉鏃犻棶棰?- **缃俊**锛氶珮锛堥€?token 鏍稿锛?
### 3.5 P2锝渆num 鍙樹綋鏋勯€犳惡甯︽樉寮忕被鍨嬪疄鍙傛椂 AST 闄嶇骇锛堥瀛橈級

- **浣嶇疆**锛歚parser/Parser.cpp` call()锛堢害 2418鈥?447 琛?R99 鍒嗘敮锛?- **绫诲瀷**锛氳竟鐣屾潯浠?- **澶嶇幇鍦烘櫙**锛歚enum Box<T> { some(T) } print(Box.some<int>(5));` 鈫?鍏ㄥ悗绔繍琛屾椂閿欒锛圡emberAccess+FunCall 闄嶇骇 AST锛夛紱`Box.some(5)` 姝ｅ父
- **鏍瑰洜**锛歟num 鍙樹綋鍒嗘敮瑕佹眰 VariantName 鍚庣揣璺?`(`锛宍<int>(` 鎺㈡祴鍦ㄥ悗缁惊鐜墠鍛戒腑锛孉ST 宸查檷绾?- **淇寤鸿**锛歟num 鍙樹綋鍒嗘敮鍦?match(TK_LPAREN) 鍓嶅厛灏濊瘯 trySkipExplicitTypeArgs()
- **缃俊**锛氫腑楂橈紙瑙ｆ瀽璺緞鐩存帴璇佹嵁锛涜繍琛屾椂鎶ラ敊鏂囨鏈疄娴嬶級

### 3.6 P2锝渄ict 閿被鍨嬪惈 `:` 鐨勬敞瑙ｅ舰鎬佹摝闄ゆ紡鍒?+ 蹇呯劧鎷掔粷锛堥瀛橈紝浣庤Е杈撅級

- **浣嶇疆**锛歚common/TypeChecker.h:201-208`锛坅nnotationReferencesTypeParam dict 鍒嗘敮鍙栭涓?`:`锛変笌 87-106锛坱ypeMatchValue 鍚屾硶锛?- **绫诲瀷**锛氳竟鐣屾潯浠?- **澶嶇幇鍦烘櫙**锛歚dict[fun():K:string]`/`dict[dict[string:K]:int]` 褰㈡€佲€斺€旈敭绫诲瀷琚涓啋鍙锋埅鏂紝鎿﹂櫎婕忓垽 + typeMatchValue 蹇呮嫆
- **淇寤鸿**锛氫笌 parseTypeAnnotation 鍚屾瀯鐨勬嫭鍙锋劅鐭ュ垏鍒嗭紙fun `(` 娣卞害銆乨ict `[` 娣卞害锛夛紝閿?鍊间袱渚у叡鐢?- **缃俊**锛氶珮锛堜袱澶勫垏鍒嗛€愯鏍稿涓€鑷村湴閿欙紱瑙﹁揪闈㈡瀬绐勨€斺€旈棴鍖?dict 浣滈敭鏈秴 DictKey 杩愯鏃惰〃绀猴級

### 3.7 P2锝滃崗绋?TcoScopeGuard 蹇収婊炵暀 fiber 鏍堬紙缃俊锛氫腑浣庯紝寰呰瘉锛?
- **浣嶇疆**锛歚interpreter/InterpreterCoroutine.cpp` coroutineFiberBody锛圱coScopeGuard 灞€閮ㄦ瀯閫狅級
- **绫诲瀷**锛氱姸鎬佹硠婕?- **鏍瑰洜锛堝緟璇侊級**锛氬畧鍗粸鐣?fiber C++ 鏍堣嚦鐢熸垚鍣ㄧ粓缁擄紱鏈熼棿 Interpreter TCO 涓婁笅鏂囧仠鐣欎簬鐢熸垚鍣ㄨ缃殑 disabled 鎬侊紝鑻ヨ功搴婂惊鐜墽琛屼腑璋冪敤 .next() 鎭㈠鏂瑰墿浣欒凯浠ｈ鍒拌绡℃敼涓婁笅鏂囷紱coroutineSuspend/callCoroutineNext 鏈繚瀛?鎭㈠ TCO 涓婁笅鏂?- **淇寤鸿**锛氭牳瀵?TcoScopeGuard 鎴愬憳涓庤功搴婃秷璐圭偣鍚庯紝灏?TCO 涓婁笅鏂囩撼鍏?coroutineSuspend 蹇収闆嗗悎锛堜笌 suspendedReturnType 鍚屾ā寮忥級
- **缃俊**锛氫腑浣庯紙鏈€愯鏍稿娑堣垂鐐癸級

### 3.8 P2锝滃崗绋嬭法绾跨▼鎭㈠缂洪槻寰★紙缃俊锛氫腑锛?
- **浣嶇疆**锛歚interpreter/InterpreterCoroutine.cpp` Linux switchToFiber锛坱_currentFiber 涓?null 鈫?swapcontext 绌烘寚閽堣В寮曠敤锛? Windows SwitchToFiber 闈?fiber 绾跨▼
- **绫诲瀷**锛氬穿婧冭竟鐣岋紙鍓嶇疆鏉′欢锛氬悓涓€ Interpreter 琚袱绾跨▼椹卞姩鈥斺€斿父瑙?GUI 鏋舵瀯鍗?worker 涓嶅彲杈撅級
- **淇寤鸿**锛?next() 鍏ュ彛鏍￠獙 fs->resumer 绾跨▼涓€鑷存€э紙thread_local 鐧昏锛夛紝涓嶄竴鑷存樉寮忔姤閿?鍗忕▼涓嶅彲璺ㄧ嚎绋嬫仮澶?
- **缃俊**锛氭満鍒剁‘瀹氾紱鍙揪鎬т綆

### 3.9 P3锝淛IT 鍧楃紦瀛樺悓閿噸澶?store 涓嶆竻鏃?LRU 鑺傜偣

- **浣嶇疆**锛歚compiler/jit/JIT.cpp:849-850`
- **绫诲瀷**锛氳祫婧愭硠婕?+ LRU 缁撴瀯鐮村潖锛堝墠缃細鍏冩暟鎹崯鍧忓洖閫€鎴栬法绾跨▼鍙?store鈥斺€旀簮鐮佽嚜鏍?涓嶅簲鍙戠敓"锛?- **淇寤鸿**锛歴tore 鍓嶆寜 key 鏌ユ棫鏉＄洰骞跺厛绉婚櫎 LRU 鑺傜偣
- **缃俊**锛氶€昏緫纭畾/鍙揪鎬т綆

### 3.10 P3锝淩egisterBytecode instructionSizeAt(REG_DEFINE_CLASS) 澶氭鎴柇鍥為€€ baseSize=7 鍙紡鎶?
- **浣嶇疆**锛歚compiler/backend-reg/RegisterBytecode.cpp` instructionSizeAt
- **绫诲瀷**锛氳竟鐣屾潯浠讹紙浠呯暩褰㈠瓧鑺傜爜鍙揪锛涙柊鏃у叡鐢ㄥ悓鍑芥暟銆佺瓑浠锋€т笉鐮村潖锛?- **淇寤鸿**锛氬娈垫埅鏂満鏅€愭鏍￠獙鎴栦繚瀹堟嫆缁濆姞杞?
### 3.11 P3锝淣aN 閿彲鍐欎笉鍙煡

- **浣嶇疆**锛歚interpreter/Value.h` dictKeyFromValue/DictKeyEqual
- **绫诲瀷**锛氳涔夎竟鐣岋紙涓夊悗绔竴鑷达紱涓?JS/Python 涓嶅悓锛涙瘡娆?set 鏂板鏉＄洰銆乬et/has 姘?miss锛?- **淇寤鸿**锛歞ictKeyIsValid 鍓嶇疆鎷︽埅 NaN 閿樉寮忔姤閿欙紝鎴栨枃妗ｅ寲

### 3.12 璁板綍椤癸紙鏃犺涓虹己闄凤級

- JIT computeBlockCacheKey 鐨?enumInfos 浠?name 鍏ラ敭锛坴ariants/paramTypes/typeParams 涓嶅叆锛夆€斺€旀湭鍙戠幇鍙揪鐨勯檲鏃ф満鍣ㄧ爜璺緞锛堝悓瀛楄妭鐮?鈬?鍚?enum 澹版槑锛夛紱寤鸿闅忎笅娆?key 缁撴瀯鍙樻洿琛ュ叏
- JIT LRU 椹遍€愮珛鍗抽噴鏀惧叡浜潡锛岃椹遍€愬悗瀛樻椿鍛戒腑瀹炰緥鎸佹偓鍨?entry鈥斺€斿畨鍏ㄤ粎闈犲崟绾跨▼鍋囪 + execute 鍏ュ彛娓呯┖锛圝IT.h 宸叉樉寮忚眮鍏嶏級锛?鍏变韩瀛樻椿"浠呰鐩栨暟鎹晶
- `Interpreter::isTypeParameter` 鎴愬憳鍑芥暟宸叉棤璋冪敤鏂癸紙涓?common/TypeChecker.h 鑷敱鍑芥暟鏋勬垚鍙屼簨瀹炴簮锛夆€斺€斿缓璁垹闄ゆ垨鏀硅浆璋冿紝闃插娲荤簿纭尮閰嶆紡鎿?- `interpreter/Value.h:302` 娉ㄩ噴"rehash 鍚庡紩鐢ㄥけ鏁?涓庢爣鍑嗕笉绗︼紙rehash 鍙け鏁堣凯浠ｅ櫒锛夆€斺€旇繃搴︿繚瀹堬紝鍙慨璁?- StackVM/RegisterVM 绾?10 澶?member-set/get 涓?remove 闈?string 鍒嗘敮鏈€忔槑鍖栤€斺€斾粎澶氫綑娣辨嫹璐濓紝琛屼负鏃犲樊寮?- PkgCliCommandTest.InstallCommandAllFromManifest 鍦ㄥ叏閲?`-j 10` 璐熻浇涓嬪嚭鐜拌繃涓€娆″け璐ワ紙exitCode 2锛夛紝鍗曡窇涓?PkgCli 38 鐢ㄤ緥骞惰 4 杞潎閫氳繃鈥斺€旂幆澧冩€?flake锛屽缓璁悗缁粰璇ュす鍏峰鍔犻噸璇曟垨瀹氫綅璐熻浇鏁忔劅鐐?
## 4. 瀹¤瑕嗙洊澹版槑

- **VM 渚у崗绋嬪揩鐓?閲嶅畾鍩烘湭瀹?*锛歝ompiler/backend-stack/VMCalls.cpp OP_YIELD 蹇収銆丷egisterVMCalls.cpp REG_YIELD 鐨勯噸瀹氬熀绠楁湳銆乼ry 鏍堟仮澶嶃€乽pvalue 妲戒綅閲嶆崟鑾封€斺€斿瓙浠ｇ悊鏃堕棿杈圭晫鍐呮湭瀹屾垚锛?*涓嶆瀯鎴?鏃犻棶棰?缁撹**銆傛湰杞慨澶嶅凡瑙﹀強涓ゅ鎸傝捣鍒涘缓鐐癸紙鏍归泦璐＄尞鍥炶皟锛変笌鍥涘蹇収涓㈠純鐐癸紙缃┖鍥炶皟锛夛紝璇ラ儴鍒嗙粡鍏ㄩ噺鍥炲綊楠岃瘉锛涢噸瀹氬熀绠楁湳鏈韩浠嶅缓璁笅杞寜 backend-consistency 搂9 checklist 涓撻」瀹¤锛屽苟浼樺厛楠岃瘉 搂3.1 鐨?VM 闂ㄦ帶鐭矾鏈哄埗
- 瀛楀吀閫忔槑鏌ユ壘/VM 鐑矾寰?GC slots_/RefCounted 鍐呭瓨搴忥細鏈疆鏃?P0/P1锛堣瑙?搂3.10-3.12 璁板綍椤癸級
- JIT tiering/OSR/IC 鍐呴儴瀹炵幇銆丄RM64 鍚庣銆佹眹缂栫紪鐮侊細鏈疆鏄庣‘鎺掗櫎

## 5. 楠岃瘉涓庣粺璁?
- 鏈疆淇 5 椤癸紝鏂板鍥炲綊 6 椤癸紙5 鏂版祴璇?+ 1 鎷嗗垎锛夛紝鍏ㄩ噺 ctest **4156/4156 閫氳繃**锛?j 10锛?-timeout 600锛孧INILANG_BUILD_PERF_TESTS=ON 鍏ㄥ彛寰勶級锛?W4+/WX 闆惰鍛?- 鏋勫缓娉ㄨ锛歚minilang_perf_test` 鐩爣鍙?`MINILANG_BUILD_PERF_TESTS` 闂ㄦ帶涓?option 瀹氫箟浜?`add_subdirectory(tests)` 涔嬪悗鈥斺€斿叏鏂?configure 蹇呴』鏄惧紡 `-DMINILANG_BUILD_PERF_TESTS=ON`锛堝凡璁板叆 AGENTS 搂6锛?- 瀹¤鏂规硶璁烘敞璁帮細鎺㈤拡瀹炴祴绾犳浜嗕袱澶勫瓙浠ｇ悊"纭畾绾?缁撹鐨勫鐜拌剼鏈紙鏈€缁?yield 鍚庤鍙ヤ负姝讳唬鐮侊紱鏁扮粍榛樿鍊艰Е鍙?BUG-DEF-1 鏃㈡湁绾︽潫锛夆€斺€?*澶嶇幇鍦烘櫙蹇呴』瀹炶窇楠岃瘉**锛岄€昏緫鎺ㄦ紨绾у鐜伴渶鏍囨敞缃俊搴?
---

# 2026-10-22 杞锛堢浜屾棩锛孷M 渚т笓椤?+ 鍗忎綔浜嬩欢璁板綍锛?
## 6. 鏈疆鎵ц鍐呭锛堟寜涓婅疆鎺ㄨ崘锛氣憼 VM 渚у崗绋嬪揩鐓?閲嶅畾鍩轰笓椤瑰璁?鈶?搂3.1 璇箟瀵归綈锛?
### 6.1 宸蹭慨澶嶏紙鏈疆锛?
- **P1锝溌?.1 娓呭伩锝淰M 閿欒閫冮€稿悗 `.next()` 璇箟瀵归綈瑙ｉ噴鍣?*锛氭満鍒跺畾浣嶏紙骞惰浼氳瘽涓庝富瀹＄嫭绔嬫敹鏁涳級= 閿欒绌块€忓悗 `vmSuspension` 宸叉秷璐广€乣done=false`銆乧d 鏃犲け璐ヨ褰曪紝涓嬫 `.next()` 鍏ㄦ柊璋冪敤鍦?fiber 妯″紡姣忛亣蹇呮寕 鈫?閲嶆寕棣?yield 鈫?`currentYieldId` 浼€掑鑷?yieldCount 鈫?鎻愬墠缃?done锛堣祴鍊肩偣 = fiber 鏈€缁?yield 鍒嗘敮锛夊苟杩斿洖棣?yield 鍊笺€備慨澶?= `CoroutineData::vmPendingErrorBox` 澶辫触淇濈暀鐩?+ 鍏ュ彛鍓嶇疆 throwException 鍚岀偣閲嶆姏锛涢€冮€稿垽鍒敤**甯ф繁搴?*鑰岄潪 tryStack 璁℃暟锛堝悗鑰呭湪鍐呴儴 try 璺ㄦ寕璧锋仮澶嶅満鏅瑙﹀彂鈥斺€擳ryCatchAcrossSuspension 瀹炴祴鍥炲綊鍚庢敼鍒わ紝涓ゆ瀹炴祴鏁欒宸茶鍏?搂9 鏉℃锛夈€俙=0` 閲嶆斁骞冲彴闈欐€佹牳瀵规湰宸蹭笌瑙ｉ噴鍣ㄩ噸鏀句竴鑷达紙闆舵敼鍔級銆傚洖褰掑崌绾у洓璺緞閿佸畾锛坄1boomboomfalse`锛夈€?- **P1锝淔1锛堜笂杞?3.1 寰呰瘉椤?W2 鍗囩骇娓呭伩锛夛綔TCO 涓婁笅鏂囪法鎸傝捣杈圭晫婊炵暀**锛氬瓙浠ｇ悊鏍稿疄鎶ュ憡璇佸疄鍘熸枃鍥涢」鏂█鍏ㄩ儴鎴愮珛锛屽苟缁欏嚭涓绘敾鍑昏矾寰勶紙缁堢粨涓婁笅鏂囧熬璋冪敤琚檲鏃?tcoName_ 璇垎绫讳负 SelfFunction 鈫?鍑芥暟浣撴惡閿欒瀹炲弬閲嶈窇锛屾帰閽堟湡鏈?56 缂洪櫡 57锛夈€備慨澶?= TCO 浜旀垚鍛樼撼鍏ユ寕璧?鎭㈠鍗忚锛坈oroutineSuspend 瀵圭О淇濆瓨/鎭㈠ + CoroutineContextGuard 閫€鍑烘仮澶嶈皟鐢ㄦ柟鐪熷€硷級锛涙帰閽堝疄娴嬩慨澶嶅悗 56銆傚洖褰?`TcoContextRestoredAcrossGeneratorTermination`銆?- **P1锝淔1锛堜笂杞?3.1 寰呰瘉椤癸級锝滄寕璧风獥鍙ｅ紑鏀?upvalue 鐩稿绱㈠紩鏆撮湶**锛圫tackVM 瀹¤鏂板彂鐜帮紝涓诲澶嶆牳閲囩撼锛夛細yield 鍑烘崟鑾风敓鎴愬櫒灞€閮ㄧ殑闂寘骞跺湪绐楀彛鏈熻皟鐢ㄢ€斺€攐pen 鎬佺浉瀵?stackSlot 鎸囧悜宸叉惉绌虹殑娲绘爤鍖哄煙锛岄潤榛樿閿欏€?鍚戣皟鐢ㄦ柟娲绘爤鍐欏叆锛堝啓璺緞鍙崌绾?P0锛夈€備慨澶?= 蹇収寮忓叧闂紙鎸傝捣鏃?`uv->value`=妲藉€?+ isClosed锛屾仮澶嶆椂浼犳挱鍐欏洖鍐嶉噸鎸?open锛屼笌璇█"鍏抽棴=蹇収"璇箟鑷唇锛屾棤鍐欏叆鏃跺箓绛夛級銆備袱 VM 鍚屽瀷銆傚洖褰?`SuspendedWindowClosureUpvalueSemantics`锛坄1111`锛夈€?- **P1锝滆法璋冪敤鏂规繁搴︽仮澶嶅惊鐜竟鐣屽洖鎷?*锛堝苟琛屼細璇濈嫭绔嬪彂鐜板苟淇锛屾彁浜?251a54c锛夛細鎭㈠璺緞閲嶅畾鍩哄悗鎶婂惊鐜竟鐣屽洖鎷ㄥ埌棣栨鎸傝捣鍏ュ彛锛屼袱娆?`.next()` 璋冪敤鏂瑰抚娣变笉鍚屽嵆瓒婄晫鎵ц璋冪敤鏂逛唬鐮?鐭矾娑堣€楀揩鐓э紱RegisterVM 琚?IR 鍐呰仈鎺╃洊锛堝叧鍐呰仈瀹炴祴鍚屽瀷锛夈€備富瀹″凡瀹￠槄 diff锛氫慨澶嶆纭紙淇濇寔鏈鍏ュ彛娣卞害銆佸揩鐓хЩ闄ょ粷瀵瑰熀绾匡級锛屽洖褰?`ResumeAtDeeper/ShallowerCallSite`銆?- **P3锝淒3 娓呭伩**锛堝苟琛屼細璇濇彁浜?46f3a6d锛夛細JIT 鍧楃紦瀛樺悓閿噸澶?store 鍏堟憳闄ゆ棫 LRU 鏉＄洰銆備富瀹″凡瀹￠槄 diff锛氭纭€?
### 6.2 鎶ュ憡鍕樿锛埪?.2 鏁扮粍瀛楅潰閲忛粯璁ゅ€硷級

鍘熻褰?瑙ｆ瀽鎴愬姛浣嗛粯璁ゅ€奸潤榛樹笉缁戝畾锛屾棤璇婃柇"**涓嶅噯纭?*銆傝拷鍔犳帰閽堝疄娴嬶紙2026-10-22锛夛細鈶?椤跺眰 `fun f(a: int[] = [1, 2])` 鈥斺€?**瑙ｆ瀽鑴辫建**锛堝嚱鏁颁綋閿欒褰掑睘锛岃繍琛屾椂鎶?return 鍙兘鍦ㄥ嚱鏁颁綋鍐呬娇鐢?锛屾棤 ParseError锛夛紱鈶?绫?init 鍚屽瀷鈥斺€旇В鏋愰€氳繃浣嗛粯璁ゅ€奸潤榛樹笉缁戝畾锛堝瓧娈?null锛夛紱鈶?鏍囬噺榛樿鍊硷紙int/T锛夊叏璺緞姝ｅ父锛涒懀 鏍囬噺瀛愪唬鐞嗘姤鍛婄殑"褰撳墠浠ｇ爜浼?ParseError"缁撹涓庡疄娴嬩笉绗︼紙鍏?CLI 鎺㈤拡涓庢祴璇曚簩杩涘埗琛屼负涓嶄竴鑷达紝鍘熷洜鏈畾浣嶏級銆傜湡瀹炴満鍒?= 鍙傛暟榛樿鍊艰〃杈惧紡鐨勬暟缁勫瓧闈㈤噺瑙ｆ瀽/姹傚€奸摼璺湪鍑芥暟澹版槑涓庣被鏂规硶涓ゆ潯璺緞涓婅涓哄垎鍙変笖鍧囦笉姝ｇ‘鈥斺€斿睘瑙ｆ瀽鍣ㄥ弬鏁伴粯璁ゅ€奸噸鏋勮寖鐣达紝缁存寔 **P2 鐧昏銆佹湰杞笉淇?*锛堥渶涓撻」杞锛夛紝BUG-DEF-1 绛栫暐涓庡疄鐜扮殑鍋忓樊涓€骞剁撼鍏ヨ杞€?
### 6.3 鍗忎綔浜嬩欢锛堝瀹炶褰曪級

鏈疆鎵ц鏈熼棿鍙戠幇**鍙︿竴骞惰浼氳瘽鍦ㄥ悓涓€宸ヤ綔鍖烘彁浜や簡涓変釜淇**锛?dd9557 = 鏈疆 搂3.1 瀵归綈鐨勪富瀹″疄鐜拌浠ｄ负鍏ュ簱锛?51a54c = 閲嶅畾鍩哄洖鎷ㄤ慨澶嶏紱46f3a6d = JIT D3锛夛紝浣滆€呰韩浠界浉鍚岋紙zwork@local锛夛紝鍐呭鍧囨簮鑷湰鎶ュ憡鐨勬帹鑽愰」涓旇川閲忕粡涓诲閫愰」瀹￠槄閲囩撼锛堝惈 54 琛屽洖褰掓祴璇曘€佹帰閽堝疄璇併€佸涓诲鎺㈤拡鐨勮瘑鍒伩璁╋級銆傛湡闂村彂鐢熶竴娆″苟鍙戞瀯寤洪攣鍐茬獊锛圠NK1168锛岄噸璇曡В鍐筹級銆?*椋庨櫓涓庡缓璁?*锛氬弻浼氳瘽骞惰鍙樻洿鍚屼竴浠撳簱瀛樺湪缂栬緫浜掕俯涓庨獙璇佸彛寰勬紓绉婚闄┾€斺€斿缓璁悗缁皢浠撳簱鍙樻洿鎿嶄綔鏀舵暃鍒板崟涓€浼氳瘽锛屾垨寤虹珛鏄庣‘鐨勪换鍔″垎宸ヨ竟鐣岋紱鏈姤鍛婁笌 CHANGELOG 宸插悎骞跺弻鏂规潯鐩€?
### 6.4 鍓╀綑鏈慨锛堟洿鏂板悗锛?
- P2锛氬弬鏁伴粯璁ゅ€兼暟缁勫瓧闈㈤噺瑙ｆ瀽鑴辫建/闈欓粯涓嶇粦瀹氾紙搂6.2锛岄渶瑙ｆ瀽鍣ㄤ笓椤硅疆锛?- P2锛氭硾鍨嬪祵濂楀嚱鏁?lambda/灞€閮?enum 涓嶇户鎵垮灞傜被鍨嬪弬鏁帮紙闇€璁捐锛?- P2锛歵rySkipExplicitTypeArgs 鐧藉悕鍗曠己 `:`/`(`锛堥渶閰嶅骞宠　璁捐锛?- P2锛歟num 鍙樹綋鏋勯€犲甫绫诲瀷瀹炲弬 AST 闄嶇骇锛汸2锛歞ict 閿被鍨嬪惈 `:` 闀垮熬锛汸2锛氬崗绋嬭法绾跨▼鎭㈠缂洪槻寰?- P3锛歂aN 閿彲鍐欎笉鍙煡锛汻egisterVM tryStack 璺ㄦ寕璧疯鏁颁笉鍙潬锛堟湰杞敼鐢ㄥ抚娣卞害鍒ゅ埆鍚庝粛寤鸿涓嬭疆鏍稿璐︾洰锛?- RegisterVM 鍗忕▼ 搂9 checklist 涓撻」瀹¤锛氬闂磋皟搴︿細璇濆凡鎸?搂9 閫愰」鏍稿瀹屾瘯锛堝崟杈瑰惎鐢ㄧ浠?蹇収瀛楁鐩稿鍖?returnIp 閲嶅畾鍩?宓屽鎸傝捣涓婁笅鏂?GC 鏍逛箟鍔?OP_RETURN try 澶勭悊鍣ㄩ殢甯у洖鏀垛€斺€斿潎纭鏃犳仚锛涜法娣卞害鍥炴嫧 P1 鍗宠杞彂鐜帮級锛汭C/tiering 鍐呴儴瀹炵幇浠嶄笉鍦ㄨ鐩栭潰
- 搂3.2 閬楃暀灏忛」锛堢櫥璁颁笉鍔級锛氣憼 BUG-DEF-1 鐧藉悕鍗曟嫆缁濆悗 synchronize 娈嬬暀 `}` 鍐嶆姤涓€鏉″浣欑殑鍙虫嫭鍙烽《灞傞敊璇紙鍙岄噸璇婃柇鍣煶锛孏UI 璇婃柇闈㈡澘鍙锛夛紱鈶?瀹芥澗 harness 涓嬫畫缂?AST 鍚勫悗绔仮澶嶅舰鎬佷笉涓€鑷粹€斺€斾袱鑰呬粎鍦ㄥ拷鐣ヨ瘖鏂殑 harness 鍙揪锛屼慨澶嶆敹鐩婁綆銆乻ynchronize 鍏ㄥ眬鍏辩敤鏀瑰姩椋庨櫓楂?
## 7. 2026-10-22 娣卞杞锛堣皟搴︿换鍔★細搂6.4 鎺ㄨ崘椤规竻鍋?+ 閫忔槑鏌ユ壘鏀跺熬锛?
> 鏈疆鎸?搂6.4 鍓╀綑娓呭崟鎵ц锛氭竻鍋?搂3.6銆伮?.11 涓庝笁鏉¤褰曢」锛堥€忔槑鏌ユ壘鏈鐩栫偣 / isTypeParameter 鍙屼簨瀹炴簮 / Value.h 娉ㄩ噴淇锛夛紝骞惰繛甯︿慨澶嶅悓鏃忔柊鍙戠幇鐨?JIT 鍙 helper COW 璇垎绂伙紙搂7.1.4锛夈€偮?.4/搂3.5锛堣В鏋愬櫒涓ら」锛夊彈闃讳簬骞冲彴鍐欏叆瀹堝崼瀵?Parser.cpp 鐨勫嚟鎹惎鍙戝紡璇垽锛屾棤娉曡惤鐩橈紙璇﹁ 搂7.2锛屼慨澶嶆柟妗堝凡瀹氫綅鍒拌銆佸緟瑙ｉ櫎鍚庝竴杞疄鏂斤級銆?
## 7.1 宸蹭慨澶嶏紙鏈疆 5 椤癸級

### 7.1.1 P2锝溌?.6 娓呭伩锝渄ict 閿被鍨嬪惈 `:` 娉ㄨВ褰㈡€佹摝闄ゆ紡鍒?+ 蹇呯劧鎷掔粷

- **浣嶇疆**锛歚common/TypeChecker.h` 鏂板 `findDictKVSeparator`/`skipTypeAnnotationFrom` + `annotationReferencesTypeParam` dict 鍒嗘敮 + `typeMatchValue` dict 鍒嗘敮
- **绫诲瀷**锛氳竟鐣屾潯浠讹紙鍥涘悗绔竴鑷存嫆缁濆悎娉曠▼搴忥級
- **澶嶇幇**锛歚fun f<K>(x: dict[dict[string:K]:int]): int { return 1; } print(f(5));` 鈫?淇鍓嶅洓鍚庣涓€鑷磋繍琛屾椂鎷掔粷锛堝唴灞傝棣栦釜 `:` 鎴柇涓?`dict[string` / `K]:int`锛屾摝闄ゆ紡鍒?鈫?杩愯鏃舵牎楠屽繀鎷掞級锛涗慨澶嶅悗 K 琚瘑鍒?鈫?鏁翠綋鎿﹂櫎 鈫?鎺ュ彈
- **鏍瑰洜**锛歚fun():K` 褰㈡€侀敭绫诲瀷鐨勮繑鍥炵被鍨嬪啋鍙蜂笌 dict 鍒嗛殧鍐掑彿鍦ㄦ嫭鍙峰鍚屾繁搴︹€斺€旂函鎷彿娣卞害鎰熺煡涓嶅彲鍖哄垎锛屽繀椤绘寜 `parseTypeAnnotation` 鍚屾瀯鏂囨硶浠庡乏**璐┆娑堣垂閿被鍨?*鍚庡畾浣嶅垎闅旂
- **淇**锛氭柊澧炴枃娉曞悓鏋勫垏鍒嗭紙dict[K:V]/fun(p):r 閫掑綊 + `[]`/`?` 鍚庣紑鍫嗗彔 + 灏鹃殢閫楀彿瀹瑰繊锛夛紝鎿﹂櫎鍒ゅ畾涓庤繍琛屾椂鏍￠獙涓や晶鍏辩敤鍚屼竴鎶婂垏鍒嗗昂锛坱ypeMatchValue 渚у鍙揪杈撳叆涓鸿涓烘棤鍙樺寲鐨勪竴鑷存€ф敹鍙ｏ級
- **鍥炲綊**锛歚GenericErasureCompletion.DictNestedDictKeyAnnotationErasure` / `DictFunKeyAnnotationErasure`锛堝洓鍚庣锛?
### 7.1.2 P3锝溌?.11 娓呭伩锝淣aN 閿彲鍐欎笉鍙煡 鈫?SameValueZero锛堝榻?JS Map/Set锛?
- **浣嶇疆**锛歚interpreter/Value.h` DictKeyHash锛堥€忔槑 double 閲嶈浇 + DictKey 鍒嗘敮锛? DictKeyEqual锛圖ictKey/DictKey銆丏ictKey/double銆乨ouble/DictKey 涓夐噸杞斤級
- **绫诲瀷**锛氳涔夎竟鐣?- **澶嶇幇**锛歚var inf = 1e308 * 10.0; var nan = inf - inf; d[nan] = 1; d[nan] = 2; print(d[nan]); print(d.len());` 鈫?淇鍓?len=2 涓?`d[nan]` 姘?miss锛圢aN 浣嶆ā寮忛殢杩愮畻鏉ユ簮涓嶅悓鏁ｅ竷涓嶅悓 hash 妗?+ equal 鍒ゅ惁锛屾瘡娆″啓鍏ユ垚涓轰笉鍙煡鎵炬鏉＄洰锛夛紱淇鍚庡啓瑕嗙洊銆佽鍛戒腑銆乴en=1锛坄21`锛?- **淇鍙ｅ緞璇存槑**锛氬師鎶ュ憡寤鸿"鍓嶇疆鎷︽埅鏄惧紡鎶ラ敊锛屾垨鏂囨。鍖?銆傛湰杞€夋嫨**鏈垪鍑虹殑绗笁鏂规 SameValueZero 瑙勮寖鍖?*锛氣憼 hash 渚ф墍鏈?NaN 瑙勮寖鍖栧埌 quiet_NaN 鍚屼竴浣嶆ā寮忥紙涓庢棦鏈?-0.0鈫?0.0 P0 fix 鍚屽瀷锛涗綅妯″紡闅忚繍绠楁潵婧愪笉鍚屾槸 equal 姘镐笉瑙﹀彂鐨勫墠鍥狅級锛涒憽 equal 渚?NaN 浜掔瓑銆傜悊鐢憋細鍗曟枃浠朵袱澶勪慨鏀瑰嵆瀵?*鍏ㄩ儴璺緞**锛堝惈 OP_BUILD_DICT 瀛楅潰閲忚矾寰勨€斺€旀姤閿欐柟妗堥渶鍙﹁琛?4 澶勫瓧闈㈤噺闂ㄦ帶锛屼笖 `kDictKeyInvalidType` 鏂囨瀵?NaN 璇锛夎嚜鍔ㄤ竴鑷达紱JS Map/Set 鍏堜緥锛涙棤鏃㈡湁鎴愬姛绋嬪簭琚柊澧炴嫆缁濄€傚睘璇箟鍙樻洿锛圢aN 閿?len 璁℃暟涓庡彲鏌ユ€у彉鍖栵級锛屽凡鍏?`backend-consistency.md` 搂2 璇箟缁嗗垯瀵圭収琛?- **娉?*锛歚0.0/0.0` 鍦?MiniLang 鏄樉寮忛櫎闆堕敊璇紙NumericUtils DivByZero 瀵?float 鍚屾牱鎷︽埅锛夛紝NaN 椤荤粡 `inf - inf` 浜х敓锛堟诞鐐逛箻/鍑忔棤妫€鏌ワ級鈥斺€攕td/math 鐨?`sqrt` 璐熸暟鍒嗘敮鍚屾牱浼氳闄ら浂鎷︽埅锛屽睘鍙︽鐧昏鐨勫簱鍐呮綔鍦ㄩ棶棰?- **鍥炲綊**锛歚ConsistencyDiff.DictNaNKeySameValueZero`锛堜笁鍚庣 `21`锛涘洓鍚庣鍏变韩 DictMap 鑷姩涓€鑷达級

### 7.1.3 璁板綍椤规竻鍋匡綔瀛楀吀閫忔槑鏌ユ壘鏀跺熬锛坢ember-set/get 9 澶?+ JIT helper 8 澶勶級

- **浣嶇疆**锛歚compiler/backend-stack/VMContainers.cpp`锛圤P_MEMBER_GET + OP_MEMBER_SET/SET_VAR/SET_LOCAL锛夈€乣compiler/backend-reg/RegisterVMExec.cpp`锛圧EG_MEMBER_GET/SET锛夈€乣interpreter/Interpreter.cpp`锛坵riteBackChain/writeBack 瀛楀吀 member-set 3 澶勶級銆乣compiler/jit/JITRuntime.cpp`锛坖itMemberGet/jitMemberGetWithIC 鍥為€€/jitMemberSetVar/jitMemberSetLocal/jitIndexGet/jitIndexSetLocal/Global/Index锛?- **绫诲瀷**锛氭€ц兘锛堣涓烘棤宸紓锛?- **鍐呭**锛氣憼 `Value.h` 鏂板 `dictFindFieldRef`/`dictGetOrInsertFieldRef`锛坰tring_view 寮傛瀯鏌ユ壘锛岄敭涓哄瓧鑺傜爜甯搁噺姹犲瓧娈靛悕锛夆€斺€斾慨澶嶅墠姣忔 `obj.field` 璇?鍐欐瀯閫?`DictKey{std::string}`锛堟瘡娆¤闂?1 娆″爢鍒嗛厤 + 鎷疯礉锛岃鐩栬祴鍊间篃涓嶄緥澶栵級锛涒憽 JIT 绱㈠紩/鎴愬憳璺緞浠?`dictKeyFromValue(Value(std::string(fieldName)))` 杩佺Щ鍒?`dictFindValue`/`dictGetOrInsertRef`/`dictFindFieldRef`鈥斺€斾慨澶嶅墠 string 閿瘡娆¤闂弻閲嶅爢鍒嗛厤锛堜复鏃?StringData + 鎷ユ湁鍨?DictKey锛?- **绾緥**锛欽IT 浠呮敼 helper 鍑芥暟浣擄紙璋冪敤绗﹀彿涓?ABI 涓嶅彉锛岀敓鎴愭満鍣ㄧ爜闆跺彉鍖栵級鈫?`kJitCodegenVersion` 涓嶉€掑

### 7.1.4 P2锛堟湰杞柊鍙戠幇锛夛綔JIT 鍙 helper 闈?const 瀹瑰櫒璁块棶瑙﹀彂 COW 璇垎绂?
- **浣嶇疆**锛歚compiler/jit/JITRuntime.cpp` jitIndexGet / jitLen / jitAddGeneric / jitOrderedCompare / jitMemberGet / jitDefineClass锛堟帴鏀惰€呭０鏄庢敼 const锛? jitIndexSetLocal/Global/Index 涓夊瓒婄晫妫€鏌ヨ锛坄std::as_const`锛? jitMemberGetWithIC dict 鍥為€€
- **绫诲瀷**锛氭€ц兘锛堝彧璇昏矾寰勮涔夋棤鍙樺寲锛?- **鏍瑰洜**锛歚bitsToValue`锛坒romBitsBorrowed锛夊鍫嗙被鍨?addRef锛孞IT 鏍堟Ы涓?helper 灞€閮?Value 骞跺瓨 鈫?refcount 鈮?2 鈫?闈?const 璁块棶鍣紙arrayVal/dictVal/stringVal/fields锛夎蛋 ensureUnique 鈫?**鍏变韩瀹瑰櫒姣忔鍙璁块棶鏁村鍣?鏁翠覆娣辨嫹璐?*锛坖itLen 鐨?len()銆佸瓧绗︿覆鎷兼帴/姣旇緝銆乣arr[i]`/`d[k]`/`obj.field` 璇诲彇鍧囧湪鍐咃級銆傚澶勬敞閲婂０绉?const 閲嶈浇锛屼笉瑙﹀彂 COW"涓庝唬鐮佷笉绗︼紱IC 鐗?helper锛坖itMemberGetWithIC 瀹炰緥鍒嗘敮锛夊凡鐢?as_const 淇繃锛屽叾浣欎负鍚屾棌閬楁紡
- **淇**锛氬彧璇?helper 鎺ユ敹鑰呬竴寰?const 澹版槑锛涘啓璺緞瓒婄晫妫€鏌ユ敼 as_const锛堜慨澶嶅墠瓒婄晫閿欒璺緞涔熷厛鏁村鍣ㄦ嫹璐濆啀鎶ラ敊锛?- **鍥炲綊**锛氬叏閲?ctest锛堣涓烘棤宸紓锛? TestJIT 涓撻」 + perf 鍩哄噯濂椾欢閫氳繃

### 7.1.5 璁板綍椤规竻鍋匡綔isTypeParameter 鍙屼簨瀹炴簮绉婚櫎 + Value.h 娉ㄩ噴淇

- `Interpreter::isTypeParameter`锛堟棤璋冪敤鏂癸級鍒犻櫎锛圛nterpreter.h/Interpreter.cpp锛夛紝闃插娲?R163 淇鍓嶇殑绮剧‘鍖归厤婕忔摝锛沗TypeChecker.h` 娉ㄩ噴鍚屾澹版槑鍞竴鍒ゅ畾鍏ュ彛涓鸿嚜鐢卞嚱鏁?+ 澶嶅悎娉ㄨВ璧?`annotationReferencesTypeParam`
- `Value.h` dictGetOrInsertRef 娉ㄩ噴"杩斿洖寮曠敤鍦?map rehash 鍚庡け鏁?涓庢爣鍑嗕笉绗︼紙rehash 浠呭け鏁堣凯浠ｅ櫒锛夆€斺€斾慨璁负鍑嗙‘琛ㄨ堪

## 7.2 鏈疄鏂斤紙鏈疆鍙楅樆/缁存寔鐧昏锛?
- **搂3.4 / 搂3.5锛圥2锛岃В鏋愬櫒涓ら」锛夊彈闃讳簬骞冲彴鍐欏叆瀹堝崼璇垽**锛欰utoClaw 宸ヤ綔鍖哄啓鍏ュ畧鍗 Parser.cpp 鍏ㄦ枃浠舵壂鎻忔椂锛屾妸鏃㈡湁浠ｇ爜/娉ㄩ噴璇垽涓哄嚟鎹瓧闈㈤噺鈥斺€斺憼 558 琛屾敞閲婏紙鍚嫳鏂囪瘝 token 涓庡啋鍙峰垎闅旂殑閿€肩姸璇存槑锛?026-06-06 鎻愪氦 9c60347e锛夛紱鈶?`const Token& tok = peek();` 绛?Token 鍨嬪０鏄庤祴鍊艰锛堝叏鏂囦欢 token+绛夊彿缁勫悎 175 琛岋紝鍚彂寮忛€愯疆涓婃姤涓嶅悓棣栧懡涓锛屾棤娉曟寜琛屾敹鏁涳級銆傛寜瀹堝崼鍗忚涓嶇粫杩囥€佷笉鏀瑰啓鏃犲叧鏃㈡湁浠ｇ爜锛涗袱椤逛慨澶嶏紙trySkipExplicitTypeArgs 鐧藉悕鍗曡ˉ `:`/`(`/`)`鈥斺€擿(` 浠呭厑璁稿墠缂€涓?TK_FUN 闃茶鍚冪畻鏈垎缁勩€乣)` 閰嶅骞宠　锛沞num 鍙樹綋鍒嗘敮鍦?match(TK_LPAREN) 鍓嶅墠缃?trySkipExplicitTypeArgs锛夊凡瀹氫綅鍒拌锛屽緟瀹堝崼鍙ｅ緞淇鍚庝竴杞惤鐩樺苟閰嶅洓鍚庣鍥炲綊
- 搂3.3 娉涘瀷宓屽鍑芥暟绫诲瀷鍙傛暟浼犳挱锛堥渶璁捐绔嬮」锛夈€伮?.8 鍗忕▼璺ㄧ嚎绋嬮槻寰★紙鍙揪鎬т綆 + 鍗忕▼瀛愮郴缁熷垰缁忎笁杞簿瀵嗕慨澶嶏紝鏈疆涓嶅啋闄╋級銆伮?.10 RegisterVM instructionSizeAt锛堜粎鐣稿舰瀛楄妭鐮佸彲杈撅級銆丣IT 缂撳瓨閿?enumInfos 琛ュ叏锛堥殢涓嬫 key 缁撴瀯鍙樻洿锛夛細缁存寔鐧昏

## 7.3 楠岃瘉涓庣粺璁?
- 鍏ㄩ噺 ctest **4163/4163 閫氳繃**锛?j 10锛?0s锛孧INILANG_BUILD_PERF_TESTS=ON 鍏ㄥ彛寰勶紝/W4+/WX 闆惰鍛婏級锛涙祴璇曡鏁?4159 鈫?4163锛?3锛歂aN 閿竴鑷存€?+ dict 娉ㄨВ鍒囧垎 脳2锛?- 琛屼负璇箟鍙樻洿 1 椤癸紙NaN 閿?SameValueZero锛屽凡鍏?backend-consistency 搂2锛夛紱鍏朵綑鍧囦负琛屼负鏃犲樊寮傜殑鎬ц兘/娓呯悊椤?- 鏋勫缓鐜娉ㄨ锛氭湰鏈?VS 浣嶄簬 `D:/vs`锛堥潪鏍囧噯 `ProgramFiles(x86)` 甯冨眬锛寁swhere 缂哄け锛夆€斺€擿scripts/_common.bat` 鐨?VS 妫€娴嬪湪瑁?shell 涓嶅彲鐢紝鏋勫缓闇€鐩存帴 `call D:\vs\VC\Auxiliary\Build\vcvars64.bat` 鍚庤皟鐢?`D:/vs/.../CMake/bin/cmake.exe`锛堟湰鏈哄疄娴嬪彛寰勶紝渚涘悗缁疆娆″鐢級

## 8. 2026-10-22 鏃ラ棿杞锛堣皟搴︿换鍔＄画浣滐細搂3.3/搂3.10 娓呭伩锛?
### 8.1 宸蹭慨澶嶏紙2 椤癸級

- **P2锝溌?.3 娓呭伩锝滄硾鍨嬪祵濂楀嚱鏁?lambda/灞€閮?enum 绫诲瀷鍙傛暟璇嶆硶浼犳挱锛堟彁浜?1001580锛?*锛氬師淇寤鸿鐨勩€岃繍琛屾椂 CallFrameGuard 鍚堝苟璋冪敤鏂瑰弬鏁般€嶇粡璋冩煡**涓嶅厖鍒?*鈥斺€斿欢杩熻皟鐢紙`make<T>()` 杩斿洖 inner 鍚庡啀璋冿級鏃惰皟鐢ㄦ柟涓婁笅鏂囧凡鏃?T锛屽姩鎬佸悎骞惰鐩栦笉鍒颁笖浼氬埗閫犳柊鍒嗘锛圴M 缂栬瘧鏈熸摝闄ゅ悗鎺ュ彈銆佽В閲婂櫒鎷掔粷锛夈€傝惤鍦颁负瀹屾暣璇嶆硶浼犳挱锛氣憼 缂栬瘧鏈?`Compiler::declareFunction` / `AstIRBuilder::emitFunctionBody` 浠?澶栧眰涓婁笅鏂?鈭?鑷韩澹版槑 鏇夸唬瑕嗙洊寮忚祴鍊硷紙lambda 鍦ㄦ硾鍨嬬被鏂规硶浣撳唴鍚屾牱涓㈠け绫荤被鍨嬪弬鏁帮紝涓€骞朵慨澶嶏級锛涒憽 杩愯鏃惰瘝娉曚簨瀹炴簮 = **闂寘鎼哄甫瀹氫箟鏈熷揩鐓?*锛坄ClosureData::definingTypeParams`锛岀函瀛楃涓插悜閲忔棤 GC 浜や簰锛孋OW 鍏变韩鏃?clone 璺緞锛夛紝涓変釜闂寘璋冪敤鐐瑰悎骞?瀹氫箟鏈?鈭?澹版槑锛宖unRegistry 鍥為€€绌哄€奸€€鍖栦负鍘熻涔夛紱鈶?灞€閮?enum 涓変晶 visitEnumDecl 骞跺叆锛坴ariant 杞借嵎鏍￠獙鐨勬摝闄ゅ垽瀹氭暟鎹簮锛?  - **瀹炴柦鏈熸崟鑾蜂袱涓湡闂**锛氣憼 IR 璺緞 `FunctionEmitCtx::savedCurrentTypeParams` 鍙湁澹版槑涓庢仮澶嶇偣銆?*浠庢棤蹇収璧嬪€?*鈥斺€斿祵濂楀嚱鏁扮紪璇戝悗鐖跺嚱鏁扮被鍨嬪弬鏁颁笂涓嬫枃鏈嵆涓㈠け锛圧163 鏃㈡湁閬楁紡锛屾硾鍨?宓屽缁勫悎姝ゅ墠鏃犳祴璇曡鐩栵級锛涒憽 璋冪敤鐐硅绫诲瀷鍙傛暟蹇呴』璧?const 璁块棶鍣ㄢ€斺€旈潪 const 鐨?`ensureUnique` 鍦ㄥ叡浜棴鍖呬笂瑙﹀彂 COW 鍏嬮殕锛岃皟鐢ㄨ繑鍥炵殑 capturedVars 鍐欏洖钀藉湪琚涪寮冪殑鍏嬮殕涓婏紝鐮村潖蹇収璇箟锛圓uditBatch6 蹇収璇箟閿佸疄娴嬪洖褰掑悗淇锛?  - **鍥炲綊 4 椤?*锛坄GenericErasureCompletion.*`锛夛細宓屽鍑芥暟绔嬪嵆璋冪敤 / 寤惰繜闂寘璋冪敤 / lambda / 灞€閮?enum 杞借嵎锛屽洓鎵ц璺緞閿佸畾
- **P3锝溌?.10 娓呭伩锝渋nstructionSizeAt 鎴柇闃插尽閽冲埗锛堟彁浜?1625439锛?*锛氫富浣撶Щ鍏ユ枃浠跺眬閮?`instructionSizeAtImpl`锛屽灞傛妸璁＄畻缁撴灉閽冲埗鍒板墿浣欏瓧鑺傛暟鈥斺€斿娈靛彉闀挎寚浠わ紙REG_DEFINE_CLASS 绛夛級鍦ㄦ缂哄け鏃跺師鎸?baseSize 鍥為€€锛宐uildIpMap/relocateGlobalSlots 绾挎€ц蛋鏌ヤ細璺冲叆鎸囦护鏁版嵁涓閿欎綅瑙ｈ銆傚悎娉曞瓧鑺傜爜鍙戝皠鍣ㄤ繚璇佹寚浠や笉瓒婄晫锛岄挸鍒朵负鏃犲彉鍖栵紱浠呯暩褰㈠瓧鑺傜爜鍙揪锛堢紦瀛?FNV 鏍￠獙鍙岄噸闃插尽锛夛紝灞炰繚瀹堝姞鍥?
### 8.2 闃诲涓庣櫥璁帮紙鏇存柊锛?
- **搂3.4/搂3.5锛圥arser.cpp 涓ら」锛夋湰杞嫭绔嬪鐜板悓涓€鍐欏叆瀹堝崼璇垽**锛氱紪杈?Parser.cpp 鏃跺畧鍗鏃㈡湁 `Token` 鍨嬪０鏄庤祴鍊艰锛坄const Token& tok = peek();` 绛夛級璇姤涓哄嚟鎹瓧闈㈤噺锛堥敭鍚?`token`锛屽€间笉鍥炴樉锛夆€斺€斾笌 搂7.2 璁板綍鍚屾簮锛屾寜瀹堝崼鍗忚涓嶇粫杩囥€佷笉鎹㈤€氶亾鍐欑洏锛涗袱椤逛慨澶嶆柟妗堢淮鎸佸凡瀹氫綅鐘舵€侊紝寰呭畧鍗彛寰勪慨姝ｅ悗涓€杞惤鐩樺苟閰嶅洓鍚庣鍥炲綊
- 鍓╀綑缁存寔鐧昏锛毬?.2锛堣В鏋愬櫒榛樿鍊间笓椤硅疆锛夈€伮?.8锛堝崗绋嬭法绾跨▼闃插尽锛夈€丣IT 缂撳瓨閿?enumInfos 琛ュ叏銆佹硾鍨嬪嚱鏁板唴澹版槑绫伙紙鏂规硶鍚堝苟绫?鏂规硶鍙傛暟锛屽灞傚嚱鏁?T 涓嶅叆鈥斺€旀湰杞寖鍥村鏂扮櫥璁帮紝涓庢硾鍨嬬被鍐呭祵濂?lambda 宸蹭慨澶嶅舰鎬佺浉閭伙級

### 8.3 楠岃瘉涓庣粺璁★紙鏈疆锛?
- 涓ょ瑪鎻愪氦锛?001580 / 1625439锛夊悇鑷嫭绔嬮€氳繃鍏ㄩ噺 ctest锛涙湯娆″彛寰?**4167/4167 閫氳繃**锛?j 10锛孧INILANG_BUILD_PERF_TESTS=ON 鍏ㄥ彛寰勶紝/W4+/WX 闆惰鍛婏級锛涙祴璇曡鏁?4163 鈫?4167锛?4 搂3.3 鍥炲綊锛?- 璇█鑳藉姏鍙樻洿 1 椤癸紙娉涘瀷宓屽浣滅敤鍩熺被鍨嬪弬鏁颁紶鎾紝姝ゅ墠鍥涘悗绔竴鑷存嫆缁濈殑鍚堟硶绋嬪簭杞负鎺ュ彈锛涢潪娉涘瀷绋嬪簭琛屼负闆跺彉鍖栤€斺€斿悎骞跺澶栧眰绌轰笂涓嬫枃涓庢棤宓屽鍦烘櫙鍧囦负鏃犲彉鍖栵級

## 9. 2026-10-22 鏃ラ棿闆嗙兢杞紙鍥涘瓙浠ｇ悊骞惰锛氬墿浣欑櫥璁伴泦涓竻鍋匡級

> 闆嗙兢妯″紡鍒嗗彂鍥涗釜鐙珛瀛愪换鍔★紙鍗忕▼绾跨▼闃插尽 / 娉涘瀷绫讳紶鎾?/ JIT 缂撳瓨閿?/ PkgCli flake锛夛紝瀛愪唬鐞嗗彧鏀圭爜涓嶆瀯寤猴紝涓讳細璇濋泦涓獙璇佷笌鎻愪氦锛涙枃浠舵墍鏈夋潈浜掓枼鍒嗗尯锛圱estCoroutine/TestGenerics/TestJIT/TestPkgCli 鍚勮嚜鐙珛锛夈€?
### 9.1 宸蹭慨澶嶏紙4 椤癸級

- **P2锝溌?.8 娓呭伩锝滃崗绋嬭法绾跨▼鎭㈠闃插尽锛堟彁浜?7b4ec10锛?*锛歚CoroutineFiberState` 鏂板 `ownerThread`锛堥娆?`.next()` 鎳掓瀯閫犳椂鐧昏鍒涘缓绾跨▼锛夛紝鎭㈠璺緞鍦ㄨЕ纰颁换浣?fiber 鏈哄埗鍓嶆牎楠屸€斺€旈潪鍒涘缓绾跨▼ `runtimeError("鍗忕▼涓嶅彲璺ㄧ嚎绋嬫仮澶?)`锛屾秷闄?Windows SwitchToFiber 璺ㄧ嚎绋嬫湭瀹氫箟琛屼负 / Linux ucontext 绌烘寚閽堝穿婧冭竟鐣屻€?*璁捐鍙栬垗**锛氫互銆屽崗绋嬧啍绾跨▼銆嶇粦瀹氭浛浠ｅ璁″缓璁殑 namespace 绾?thread_local锛堝悗鑰呬粎璁板綍绾跨▼鏈€杩?fiber锛岀粦瀹氳涔夊急锛夈€?*娴嬭瘯鍙揪鎬ц皟鏌?*锛氳瑷€灞備笉鍙揪鈥斺€攕pawn 涓哄欢杩熸墽琛屾ā寮忥紙join 鏃跺悓涓€绾跨▼鍚屾鎵ц闂寘锛屾棤绗簩涓?OS 绾跨▼椹卞姩 Interpreter锛夛紝鍥炲綊鐢ㄤ緥缁?C++ `std::thread` 涓ユ牸涓茶椹卞姩纭畾鎬у懡涓紙`CoroutineSuspension.CrossThreadResumeRejected`锛孧INILANG_CORO_FIBER 闂ㄦ帶锛?- **P2锝滄硾鍨嬪嚱鏁板唴澹版槑绫绘竻鍋匡紙鎻愪氦 50a49b7锛屄?.3 鍚屾棌鏀跺熬锛?*锛氫笁渚у０鏄庣偣骞跺叆瀹氫箟鏈熶笂涓嬫枃鈥斺€擿CompilerClass::emitMethodBody`锛坓uard.saved.currentTypeParams 鈭?绫诲弬鏁?鈭?鏂规硶鍙傛暟锛夈€乣AstIRBuilder::visitClassDecl`锛坈ompilingClassTypeParams_ 骞跺叆 currentTypeParams_锛夈€佽В閲婂櫒 `ClassInfo.typeParams` 骞跺叆锛坕nvokeMethod/init 鍚堝苟浠?ClassInfo 涓轰簨瀹炴簮锛夈€傛鍓?`fun f<T>` 鍐呭０鏄庣被鐨勫瓧娈?鏂规硶娉ㄨВ鍥涜矾寰勪竴鑷存嫆缁濓紱椤跺眰绫?闈炴硾鍨嬪満鏅浂鍙樺寲銆傚洖褰?2 椤癸紙澶嶇幇鍦烘櫙 + 鏂规硶鑷甫 U 涓夋柟鍚堝苟锛?- **P3锝溌?.12 璁板綍椤规竻鍋匡綔JIT 鍧楃紦瀛橀敭 enumInfos 鍏ㄩ噺鍏ラ敭锛堟彁浜?38ee8fc锛?*锛氳ˉ鍏?name/variants锛堝悕+arity+鍙傛暟绫诲瀷锛?typeParams 娣峰叆锛沗kJitCodegenVersion` 1鈫?锛堟満鍣ㄧ爜閫愪綅涓嶅彉闈炴纭€у繀闇€锛屼繚瀹堥€掑鎵挎媴閿唬闄呯‘瀹氭€ч殧绂诲苟涓?ADR-008 闃舵 3 纾佺洏鎸佷箙鍖栫珛銆岄敭缁撴瀯鍙樻洿鍚屾閫掑銆嶅厛渚嬶級銆傚洖褰?`BlockCacheKeyDistinguishesEnumDeclarations`锛堝悓瀛楄妭鐮佷笉鍚?enum 澹版槑 鈫?閿笉鍚岋級
- **P3锝溌?.12 娓呭伩锝淧kgCli 鍏变韩 CWD lockfile 绔炰簤 flake锛堟彁浜?4586759锛?*锛氭牴鍥犻摼瀹屾暣瀹氫綅鈥斺€攍ockfilePath 鏈缃椂榛樿 CWD 鐩稿 `minilang.lock`锛坈li/pkg_core.h:188锛? gtest_discover_tests 姣忕敤渚嬬嫭绔嬭繘绋嬪苟琛屽叡浜?CWD + saveLockfile 鎴柇鍐欏叆闈炲師瀛?loadLockfile 鍗婃埅 JSON 瑙ｆ瀽澶辫触 鈫?鍋跺彂 exitCode 2銆? 澶勫懡浠ょ骇鐢ㄤ緥闅旂鍒板悇鑷复鏃剁洰褰曪紙涓嶆敼鍙樿鏂█琛屼负锛夛紱楠岃瘉 PkgCli 38/38 脳 4 杞?-j 10 鍏ㄧ豢

### 9.2 鏂扮櫥璁帮紙鏈疆瀛愪换鍔″彂鐜帮級

- 鍗忕▼鍊艰法绾跨▼鏋愭瀯鐨?DeleteFiber 杈圭晫锛堣В閲婂櫒鐢熷懡鍛ㄦ湡渚э紝瓒?搂3.8 鑼冨洿锛涘彲杈炬€т綆浜庤法绾跨▼鎭㈠锛?- 浜у搧绾э細`saveLockfile` 鏀逛复鏃舵枃浠?rename 鍘熷瓙鏇挎崲锛坈li/ 渚э紝鏍归櫎鎵€鏈夎皟鐢ㄦ柟鍗婃埅璇伙紱鐢ㄦ埛浠撳簱澶氱粓绔満鏅悓鏋勶級

### 9.3 鍓╀綑鏈慨娓呭崟锛堟洿鏂板悗锛屽凡澶у箙鏀舵暃锛?
- 搂3.2锛堣В鏋愬櫒鍙傛暟榛樿鍊间笓椤硅疆锛夈€伮?.4/搂3.5锛坱rySkipExplicitTypeArgs 鐧藉悕鍗?/ enum 鍙樹綋鏄惧紡绫诲瀷瀹炲弬锛夆€斺€斾笁鑰呭潎鍦?parser/Parser.cpp锛岃鍐欏叆瀹堝崼鍑嵁鍚彂寮忚鍒ら樆濉烇紙涓よ疆鐙珛澶嶇幇锛夛紝寰呭畧鍗彛寰勪慨姝ｅ悗涓€杞惤鐩?- 搂3.2 閬楃暀灏忛」锛氱櫧鍚嶅崟鎷掔粷鍚?synchronize 娈嬬暀鍙岄噸璇婃柇鍣煶銆佸鏉?harness 娈嬬己 AST 鎭㈠褰㈡€佸垎姝э紙浠呭拷鐣ヨ瘖鏂殑 harness 鍙揪锛?- 搂9.2 涓ゆ潯鏂扮櫥璁?- 鏃╂湡璁板綍椤圭淮鎸侊細JIT GVN undo-log / Token lexeme string_view / 琛屽垪鍏冩暟鎹?RLE 绛夛紙鍘嗚疆璇勪及缁撹瑙?CHANGELOG 褰掓。锛?
## 10. 2026-10-22 鏃ラ棿缁綔杞紙闆嗙兢妯″紡锛? 瀛愪唬鐞嗗苟琛屽璁?+ 涓讳細璇濅慨澶嶄笌楠岃瘉锛?
> 浠诲姟闈細鈶犳竻鍋垮墿浣欑櫥璁?鈶″叏椤圭洰浼樺寲鏈轰細鎺掓煡銆? 瀛愪唬鐞嗗苟琛岋紙interpreter 鎬ц兘 / 鍙?VM 鍚庣鎬ц兘 / JIT+鍓嶇鎬ц兘 / gui+cli+鍗忕▼鐢熷懡鍛ㄦ湡鍋ュ．鎬?/ 搂3.2 鍙傛暟榛樿鍊间笓椤硅皟鏌ワ級锛屽瓙浠ｇ悊鍙涓嶆瀯寤猴紝涓讳細璇濋泦涓慨澶嶃€侀獙璇佷笌鎻愪氦銆?
### 10.1 宸蹭慨澶嶏紙鏈疆 2 椤癸級

- **P1锝滃洓鍚庣涓嶄竴鑷达紙鏈疆鏂板彂鐜板嵆娓呭伩锛屾彁浜?6f970ad锛夛綔椤跺眰宓屽鍧楅伄钄藉€间笉鎭㈠**锛歵est_harness/diff_test V10 瀹炶瘉鈥斺€擿var x=1; { var x=2; { var x=3; print(x); } print(x); } print(x);` 瑙ｉ噴鍣?`3,2,1` vs StackVM锛堥潪 IR 鐩磋瘧璺緞锛塦3,3,1`锛涜 harness 涓嶅湪 ctest 鍙ｅ緞鍐咃紝鍘嗚疆 4171 鍏ㄧ豢鏈嫤銆傛牴鍥狅細椤跺眰鍧楅伄钄藉崗璁紙visitBlock H1/B4锛変腑锛岀鍏堝潡 removeMapping 鍚庤繍琛屾椂 globalNameToSlot_ 蹇矾寰勪粛鎸囧悜鍘熸Ы浣嶏紙缂栬瘧鏈熺Щ闄や笉鍚屾杩愯鏃舵槧灏勶級锛屽祵濂楀潡 pre-pass 鍥?lookupGlobalSlot<0 璺宠繃淇濆瓨 鈫?鍐呭眰 `var x` 缁忓悕绉?op 姹℃煋鍘熸Ы涓旈€€鍑轰笉鎭㈠锛涘崟灞傚潡锛圴9锛夊洜鍏ㄩ儴璇诲啓鍚屾Ы鑰屽伓鐒舵纭€備慨澶?= `Compiler::shadowedGlobalNames_` 鐧昏娲昏穬鍧楃Щ闄ょ殑鏄犲皠鍚嶏紝宓屽鍧?pre-pass 瀵圭鍏堥伄钄藉悕缁忓悕绉?op锛圤P_GET_VAR锛変繚瀛樺綋鍓嶆湁鏁堝€硷紝閫€鍑鸿蛋鏃㈡湁 SET_VAR 鎭㈠璺緞锛堝榻?AstIRBuilder 椤跺眰鍧?CRITICAL-3 鐨?GLOBAL_NAME 璺緞锛汭R 璺緞姝ゅ墠宸蹭慨鏁呬笁璺湰灏变竴鑷达級銆傚洖褰掞細ConsistencyDiff 鏂板 `runStackVM_Direct`锛堥潪 IR 鐩磋瘧鍙ｅ緞锛屽浠跺師鏈?helper 浠呰鐩?IR 璺緞锛? 閬斀鏃?4 鐢ㄤ緥 脳 鍥涜矾锛堣В閲婂櫒/闈?IR 鏍堝紡/IR 鏍堝紡/IR 瀵勫瓨鍣級閿佸畾锛沝iff_test V10 杞?PASS銆?*鍚屾棌鏈慨鐧昏**锛氶《灞?catch 鍙橀噺閬斀锛坮estoreCatchScope removeMapping锛夊悗鐨勫祵濂楀潡鍦烘櫙鍚屾瀯锛岃Е杈炬洿浣庯紝闅忎笅杞€?- **P2锛堜骇鍝佺骇锛夛綔搂9.2 娓呭伩锛堟彁浜?0278078锛夛綔pkg writeFile 鍘熷瓙鍖?*锛氬悓鐩綍涓存椂鏂囦欢锛坄.tmp<pid>`锛? `std::filesystem::rename` 鏇挎崲锛圵indows MSVC STL = MOVEFILE_REPLACE_EXISTING锛孭OSIX rename 鍘熷瓙鏇挎崲锛夛紝rename 澶辫触鍥為€€灏卞湴鍐欍€俿aveManifest/saveLockfile 涓よ皟鐢ㄦ柟涓€骞跺彈鐩娾€斺€斿崐鎴?JSON 骞跺彂璇绘牴鍥犻摼婧愬ご鏀跺彛锛埪?.12 娴嬭瘯闅旂鍙不鏍囷級銆傚洖褰?`PkgLockfileTest.SaveLockfileAtomicReplaceNoTempResidue`锛堝師瀛愯鐩?+ 鍐呭瀹屾暣 + 鏃?.tmp 娈嬬暀锛夈€?
### 10.2 鍙楅樆缁存寔鐧昏锛堝啓鍏ュ畧鍗涓夋璇垽锛岀嫭绔嬪鐜帮級

- **搂3.4 / 搂3.5 / 搂3.2锛圥arser.cpp 涓夐」锛夋湰杞啀娆″皾璇曡惤鐩樿鎷?*锛氱紪杈?Parser.cpp 鏃跺畧鍗鏃㈡湁 `Token` 鍨嬪０鏄庤祴鍊艰锛堥敭鍚?`token`锛屽€间笉鍥炴樉锛夎鎶ュ嚟鎹瓧闈㈤噺鈥斺€斾笌鍓嶄袱杞紙搂7.2/搂8.2锛夊悓婧愶紝鎸夊畧鍗崗璁笉缁曡繃銆?*鏈疆澧為噺**锛毬?.2 涓撻」璋冩煡瀹屾垚锛坅udit-notes-20261022/parser-default-value-investigation.md锛夛紝鐮存 搂6.2 鎮鈶ｂ€斺€擿Parser::parse()` 鎭掕繑鍥?Block 涓旀祴璇曚晶 runner锛坈ommon/ThreeBackends.h 鍥涘悗绔?helper锛変笉妫€鏌?`hasErrors()`銆丆LI 渚ф鏌ワ紝涓や晶瑙傚療宸紓鍏ㄧ敱姝ゆ潵锛岃В鏋愬櫒琛屼负鏈竴鑷达紱鏍瑰洜閾惧畬鏁村畾浣嶅埌 parseParamList 鐧藉悕鍗曟姏鍑哄悗鐨?synchronize 鎭㈠钀界偣鍒嗗弶锛堥《灞傦細`)`/`{` 琚綋鏅€?token 娑堣€?鈫?鍑芥暟浣撴硠婕忛《灞傦紱绫昏矾寰勶細鍋滃湪 init 浣?`}` 鈫?parseClassMembers 璇垽绫诲熬 鈫?init 闈欓粯涓㈠純锛夈€備笁椤逛慨澶嶆柟妗堝潎宸插畾浣嶅埌琛?+ 鐢ㄤ緥娓呭崟灏辩华锛屽緟瀹堝崼鍙ｅ緞淇鍚庝竴杞惤鐩樸€?
### 10.3 鏂扮櫥璁帮紙鏈疆瀛愪换鍔″彂鐜帮紝鎸変弗閲嶆€э級

- **P1锛堟帹婕旓紝楂樼疆淇★級锝淕cManager 鍗曟Ы瑙﹀彂鍥炶皟瑕嗙洊 + sweep 鏃?refCount 杩囨护 鈫?瀛樻椿瀹瑰櫒璇洖鏀堕潰**锛欱ackendExecutionService::execute 姣忔鏍堟瀯閫犱复鏃?Interpreter 瑕嗙洊杩涚▼绾у崟渚?gcTriggerCallback_锛堝崟妲斤級锛屼竷闈㈡澘锛圔ackendParallel/BackendCompare/BugHunt/CoroutineVisualizer/ExerciseGrader/PerformanceRace/ProfileDashboard锛夎繍琛屾湡鏃?isRunning 瀹堝崼鐩存帴瑙﹀彂鈥斺€斾复鏃跺疄渚嬫牴闆?sweep 浼氭竻绌哄父椹?Worker 瑙ｉ噴鍣?globalEnv_ 鎸佹湁鐨勫瓨娲诲鍣紙GcOnly 妯″紡鐩存帴 delete 鈫?UAF 闈級锛岄檮甯?owner 浠ょ墝浣垮父椹诲疄渚嬪閲?GC 姘镐箙闈欓粯澶辨晥銆侴cVisualizerPanel 鑷韩鏈?isRunning 瀹堝崼 + 娉ㄩ噴鑷瘉璇ヤ笉鍙橀噺銆傛渶灏忎慨澶?= 涓冮潰鏉垮叆鍙ｇ粺涓€鍔犲畧鍗紱褰诲簳淇 = 澶氭Ы鍥炶皟/鏍归泦鍚堝苟銆備綅缃笌瑙﹀彂閾捐瑙?docs/audit-notes-20261022/robustness-gui-cli-lifetime.md F1銆?- **P1锛堟帹婕旓紝楂樼疆淇★級锝淩EPL 璺ㄨ鐢熸垚鍣ㄦ仮澶嶅繀鐒跺け璐?*锛歊eplPanel 姣忚鐙珛 std::async 绾跨▼ + 搂3.8 ownerThread 闃插尽锛?b4ec10锛夆啋 浠讳綍璺ㄨ鎸傝捣/鎭㈠鐢熸垚鍣ㄥ繀鐒跺懡涓€屽崗绋嬩笉鍙法绾跨▼鎭㈠銆嶃€傞槻寰℃纭紝瀹夸富绾跨▼妯″瀷涓庡叾鍐茬獊銆備慨澶嶆柟鍚?= REPL 浼氳瘽缁戝畾鍗曚竴鎵ц绾跨▼锛堜笌 搂10.1 瀹夸富渚?toString 绾跨▼鏀归€犲悎骞惰璁★級銆傝瑙佸悓鏂囦欢 F2銆?- **P2锛堟帹婕旓紝楂樼疆淇★級锝溌?.2 涓讳綋娓呭伩璋冩煡瀹屾垚锝滃崗绋嬪€艰法绾跨▼鏋愭瀯 DeleteFiber 杈圭晫**锛欴eleteFiber 鎵ц绾跨▼ = CoroutineData 鏈€鍚庝竴娆￠噴鏀剧嚎绋嬶紙RefCounted atomic 褰掗浂渚э級锛孖DE 鍙揪锛圧eplPanel future.get() 涓荤嚎绋嬫瀽鏋勬寕璧峰崗绋?Value锛況estoreReplState 涓荤嚎绋嬩涪寮?run-scoped globalEnv_ 鍚屾瀯锛夈€傚悗鏋?= Windows 鏈枃妗ｅ寲璺ㄧ嚎绋?DeleteFiber + fiber 鏍堟硠婕忥紙绾?4MB/鍗忕▼锛岄暱浼氳瘽绱Н锛夛紱鏃?UAF锛堝彧鍛戒腑 parked fiber锛夛紱璇█灞?capi/cli 涓嶅彲杈俱€傛渶灏忎慨澶?= ~CoroutineFiberState ownerThread 姣斿锛岃法绾跨▼鏈夋剰寮冪疆骞跺悓姝?ADR-007 Known Limitation锛涘涓讳晶鏀?worker 绾跨▼ toString銆傝瑙佸悓鏂囦欢 搂1銆?- **P2锛堝緟璇侊紝闇€宸垎楠岃瘉锛夛綔JIT methodCallIC 浠ュ厠闅嗕复鏃朵覆 c_str() 鍦板潃涓洪敭**锛氬厠闅嗗昂瀵告亽瀹氬嵆閲婂嵆鍒嗭紝涓嶅悓绫诲悕楂樺害鍙兘鍚岄敭 鈫?澶氭€佽皟鐢ㄧ偣鍋囧懡涓?閿欒鏂规硶鍒嗗彂銆傚缓璁敼 InstanceData* 閿€傚悓鏂囦欢 jitMethodCall 姣忔鏂规硶璋冪敤鏁村疄渚?COW 鍏嬮殕锛堝悓鏃?jitMemberGetWithIC 宸蹭慨锛変负纭绾ф€ц兘椤广€傝瑙?docs/audit-notes-20261022/perf-jit-frontend.md銆?- **P2锝淰M 椤跺眰鍧楀唴鏂板悕娉勬紡锛堟湰杞?diff_test V11 鎺㈤拡瀹炶瘉锛?*锛歚{ var y = 5; }` 鍧楅€€鍑哄悗 VM锛堥潪 IR 鐩磋瘧璺緞锛変粛鍙 y锛坴isitVarDecl 椤跺眰鍒嗘敮涓嶅叆 currentLocals_ 鈫?visitBlock blockVars 涓虹┖ 鈫?鏃?DELETE_VAR锛夛紝瑙ｉ噴鍣ㄦ姤銆屾湭瀹氫箟鐨勫彉閲? y銆嶃€侷R 璺緞琛屼负寰?V11 鍥涜矾澶嶆牳锛堝叾椤跺眰 for 宸叉湁 BUG-IR-SCOPE-1 娓呯悊鍏堜緥锛屾櫘閫氬潡鍚屾棌缂哄け锛夈€備慨澶嶆柟鍚?= 鍧楀唴 fresh 鍏ㄥ眬鍚嶇櫥璁?+ 鍧楅€€鍑?DELETE_VAR/DELETE_GLOBAL锛堝榻?for 寰幆鍏堜緥锛夛紱娉ㄦ剰椤绘帓闄ゅ凡鐭ユЫ浣嶅悕涓庣鍏堥伄钄藉悕锛堥伩鍏嶇牬鍧?V9/V10 璇箟锛夈€俤iff_test V11 淇濈暀涓虹孩鍩哄噯銆?- **P3锝渃li 灏卞湴鎴柇鍥炲啓**锛歠mt_core.cpp:101 / lint_core.cpp:336锛?-write 灏卞湴鎴柇鍐欑敤鎴锋簮鏂囦欢锛夈€乨oc.cpp:75 / main.cpp:295 / fuzz_core.cpp:1139 瀵煎嚭闈炲師瀛愨€斺€斿悓 搂10.1 writeFile 妯″紡鍙粺涓€鏀跺彛銆?- 鍚屾棌璁板綍锛欼R 璺緞 GVN undo-log銆乀oken lexeme string_view锛圱oken 鈮?8B锛屾敼 string_view -18% 浣?Formatter::setComments 鎸夊€兼嫹璐?Token 绛夊洓灞傜敓鍛藉懆鏈熸尝鍙婏紝闇€绔嬮」锛夈€佽鍒楀厓鏁版嵁 RLE锛?*璇勪及缁撹锛氫笉閫傜敤鍏抽棴**鈥斺€攍ine/col 宸插唴鑱?Token锛屾棤鐙珛鏁扮粍鍙帇锛夈€?
### 10.4 浼樺寲鏈轰細鐩綍锛堟湰杞?5 璺璁′骇鍑猴紝鍧囧畾鎬с€屽緟璇侊紙鏈疄娴嬶級銆嶏級

- `docs/audit-notes-20261022/perf-interpreter.md`锛?0 鏉°€傛渶楂樹环鍊?= 鈶?COW 浼叡浜紙collectAndEvaluateChain 閾炬牴鎸夊€兼嫹鍏?vals 鈫?rc鈮? 鈫?姣忔瀹瑰櫒鍙樺紓鍏ㄩ噺鍏嬮殕锛屽惊鐜?push O(N虏)锛孷arRef 鍗曡妭鐐归摼鍘熶綅鍙樺紓鍙秷闄わ紝闆惰涓哄彉鍖栵級锛涒憽 姣忚鍙?姣忚皟鐢ㄦ棤鏉′欢 debugger() mutex+寮曠敤璁℃暟锛坅tomic 鐭矾鍏堜緥 checkBreak 宸叉湁锛夛紱鈶?瀛楃涓插瓧闈㈤噺姣忔姹傚€煎爢鍒嗛厤 StringData锛圓ST 鑺傜偣缂撳瓨锛夈€?- `docs/audit-notes-20261022/perf-compiler-backends.md`锛?3 鏉★紙鎵ц鏈?A1-A7 + 缂栬瘧鏈?B1-B6锛夈€傞珮浠峰€?= A1 鏂规硶杩斿洖瀛楁鍐欏洖鏁翠綋璧嬪€硷紙RegVM directReplace 宸叉湁鏈悓姝ワ級銆丄2 甯ф瀯閫?fieldCount 娆″瓧绗︿覆 find銆丄3 RegisterVM 鍚嶇О鐗堝叏灞€鏃?varCache銆丅1 IR CSE string 閿€肩紪鍙锋瘡鎸囦护 to_string銆?- `docs/audit-notes-20261022/perf-jit-frontend.md`锛? 鏉°€傞珮浠峰€?= jitMethodCall 鏁村疄渚?COW 鍏嬮殕锛堢枒涓?IC 閿棶棰樺悓鐐逛慨澶嶏級銆乯itGetVar/SetVar 涓存椂涓叉煡琛紱lexer/parser/formatter 椤硅鏂囦欢銆?- `docs/audit-notes-20261022/robustness-gui-cli-lifetime.md`锛欶1-F4 + 姝ｉ潰缁撹锛坕de.cpp 闃叉姈/澧為噺楂樹寒/Worker 娓呯悊閾炬棤鏂板彂鐜帮級銆?
### 10.5 楠岃瘉涓庣粺璁?
- 涓ょ瑪鎻愪氦锛?f970ad / 0278078锛夊悇鑷€氳繃瀹氬悜鍥炲綊鍚庡悎骞堕獙璇侊細鍏ㄩ噺 ctest **4176/4176 閫氳繃**锛?j 10锛?02s锛?-timeout 600锛孧INILANG_BUILD_PERF_TESTS=ON 鍏ㄥ彛寰勶級锛?W4+/WX 闆惰鍛婏紱娴嬭瘯璁℃暟 4171 鈫?4176锛?4 閬斀鏃忓洓璺洖褰?+1 lockfile 鍘熷瓙鍐欙級銆?- 鏋勫缓鐜娉ㄨ锛堝鐢?搂7.3 鍙ｅ緞锛夛細鏈満瑁?shell 鏃?cmake/ctest 涓?VS 闈炴爣鍑嗗竷灞€鈥斺€旀瀯寤洪』 `call D:\vs\VC\Auxiliary\Build\vcvars64.bat` 鍚庤皟鐢?`D:\vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`锛坈test.exe 鍚岀洰褰曪級锛涜８ cmake 鐩存帴鏋勫缓鎶?C1083 鏍囧噯澶存壘涓嶅埌锛圛NCLUDE 鐜缂哄け锛夛紝鎺㈤拡杩愯闇€ `D:\qt\6.10.3\msvc2022_64\bin` 鍏?PATH銆?- 瀹¤鏂规硶璁烘敞璁帮細鏈疆鍐嶆楠岃瘉銆屽鐜板満鏅繀椤诲疄璺戙€嶁€斺€斿啓鍏ュ畧鍗笁娆¤鍒ら樆鏂殑 Parser.cpp 涓夐」鏀圭敱鍙璋冩煡鎺ㄨ繘鑷冲疄鏂藉氨缁紱diff_test V10/V11 涓や釜瀹炶瘉鍙戠幇鍧囦笉鍦?ctest 鍙ｅ緞鍐咃紙harness 涓嶆敞鍐?gtest锛夛紝寤鸿涓嬭疆璇勪及鎶?diff_test 绾冲叆 ctest 鍙ｅ緞鎴栧皢鍏剁敤渚嬭縼鍏?ConsistencyDiff銆?
