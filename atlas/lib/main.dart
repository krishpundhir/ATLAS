// ATLAS - Flutter Android controller for the ESP32-S3 tennis ball launcher.
// All application code lives in this file. See README.md for the JSON protocol.
//
// Dependencies (pubspec.yaml):  web_socket_channel ^3.0.1, shared_preferences ^2.3.2
import 'dart:async';
import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:web_socket_channel/web_socket_channel.dart';

// ============================================================================
// GLOBALS / HELPERS
// ============================================================================
final GlobalKey<ScaffoldMessengerState> messengerKey = GlobalKey<ScaffoldMessengerState>();
final AtlasLink link = AtlasLink();
final DrillStore store = DrillStore();

double _d(dynamic v, [double def = 0]) => v is num ? v.toDouble() : def;
int _i(dynamic v, [int def = 0]) => v is num ? v.toInt() : def;
bool _b(dynamic v, [bool def = false]) => v is bool ? v : def;
String _fmt(double v, [int dec = 0]) => v.toStringAsFixed(dec);

void snack(String msg, {bool error = false}) {
  messengerKey.currentState
    ?..hideCurrentSnackBar()
    ..showSnackBar(SnackBar(
      content: Text(msg),
      backgroundColor: error ? Colors.red.shade800 : null,
      duration: Duration(seconds: error ? 5 : 2),
    ));
}

Future<bool> confirm(BuildContext c, String title, String body) async {
  final r = await showDialog<bool>(
    context: c,
    builder: (_) => AlertDialog(
      title: Text(title),
      content: Text(body),
      actions: [
        TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('Cancel')),
        FilledButton(onPressed: () => Navigator.pop(c, true), child: const Text('Proceed')),
      ],
    ),
  );
  return r ?? false;
}

Future<void> messageDialog(BuildContext c, String title, String body) => showDialog<void>(
      context: c,
      builder: (_) => AlertDialog(
        title: Text(title),
        content: SingleChildScrollView(child: Text(body)),
        actions: [TextButton(onPressed: () => Navigator.pop(c), child: const Text('OK'))],
      ),
    );

// ============================================================================
// MODELS
// ============================================================================
class Step {
  double elev, yaw, m1, m2, spin;
  int intervalMs, reps, pauseAfterMs;
  bool shared, feed, waitStable, requirePos, holdAfter;
  Step({
    this.elev = 25, this.yaw = 0, this.m1 = 4000, this.m2 = 4000, this.spin = 0,
    this.intervalMs = 3000, this.reps = 1, this.pauseAfterMs = 0,
    this.shared = true, this.feed = true, this.waitStable = true,
    this.requirePos = false, this.holdAfter = false,
  });
  Step copy() => Step(
      elev: elev, yaw: yaw, m1: m1, m2: m2, spin: spin, intervalMs: intervalMs, reps: reps,
      pauseAfterMs: pauseAfterMs, shared: shared, feed: feed, waitStable: waitStable,
      requirePos: requirePos, holdAfter: holdAfter);
  Map<String, dynamic> toJson() => {
        'elev': elev, 'yaw': yaw, 'm1': m1, 'm2': m2,
        if (shared) 'speed': m1,
        'spin_rpm': spin, 'interval_ms': intervalMs, 'reps': reps, 'pause_after_ms': pauseAfterMs,
        'feed': feed, 'wait_stable': waitStable, 'require_position': requirePos,
        'next': holdAfter ? 'pause' : 'auto', 'shared': shared,
      };
  factory Step.fromJson(Map<String, dynamic> j) => Step(
        elev: _d(j['elev']), yaw: _d(j['yaw']), m1: _d(j['m1']), m2: _d(j['m2']), spin: _d(j['spin_rpm']),
        intervalMs: _i(j['interval_ms'], 3000), reps: _i(j['reps'], 1), pauseAfterMs: _i(j['pause_after_ms']),
        shared: _b(j['shared'], true), feed: _b(j['feed'], true), waitStable: _b(j['wait_stable'], true),
        requirePos: _b(j['require_position']), holdAfter: j['next'] == 'pause',
      );
  String summary() =>
      'elev ${_fmt(elev, 1)}°  yaw ${_fmt(yaw, 1)}°  W1 ${_fmt(m1 + spin / 2)}  W2 ${_fmt(m2 - spin / 2)} RPM  every ${(intervalMs / 1000).toStringAsFixed(1)} s ×$reps';
}

class Drill {
  String id, name;
  int cycles, cyclePauseMs;
  List<Step> steps;
  bool preset;
  Drill({required this.id, required this.name, this.cycles = 1, this.cyclePauseMs = 0, required this.steps, this.preset = false});
  Drill copy({String? id, String? name, bool? preset}) => Drill(
      id: id ?? this.id, name: name ?? this.name, cycles: cycles, cyclePauseMs: cyclePauseMs,
      steps: steps.map((s) => s.copy()).toList(), preset: preset ?? this.preset);
  int get totalShots => cycles * steps.fold<int>(0, (a, s) => a + s.reps);
  // Rough duration estimate (excludes spin-up, positioning and feeder time).
  int get estMs {
    final one = steps.fold<int>(0, (a, s) => a + s.intervalMs * s.reps + s.pauseAfterMs);
    return one * cycles + cyclePauseMs * (cycles - 1);
  }
  Map<String, dynamic> toJson() => {
        'id': id, 'name': name, 'cycles': cycles, 'cycle_pause_ms': cyclePauseMs,
        'steps': steps.map((s) => s.toJson()).toList(),
      };
  factory Drill.fromJson(Map<String, dynamic> j) => Drill(
        id: j['id'] ?? 'u_${DateTime.now().millisecondsSinceEpoch}', name: j['name'] ?? 'Drill',
        cycles: _i(j['cycles'], 1), cyclePauseMs: _i(j['cycle_pause_ms']),
        steps: ((j['steps'] as List?) ?? []).map((e) => Step.fromJson(Map<String, dynamic>.from(e))).toList(),
      );
}

String newId() => 'u_${DateTime.now().millisecondsSinceEpoch}';

List<Drill> buildPresets() => [
      Drill(id: 'preset_straight', name: 'Straight consistency', preset: true, steps: [
        Step(elev: 25, yaw: 0, m1: 4500, m2: 4500, intervalMs: 3000, reps: 10),
      ]),
      Drill(id: 'preset_lr', name: 'Alternating left / right', preset: true, cycles: 3, steps: [
        Step(elev: 25, yaw: -15, m1: 4000, m2: 4000, intervalMs: 4000),
        Step(elev: 25, yaw: 15, m1: 4000, m2: 4000, intervalMs: 4000),
      ]),
      Drill(id: 'preset_highlow', name: 'High and low trajectory', preset: true, cycles: 3, steps: [
        Step(elev: 15, yaw: 0, m1: 5000, m2: 5000, intervalMs: 3500),
        Step(elev: 45, yaw: 0, m1: 4500, m2: 4500, intervalMs: 3500),
      ]),
      Drill(id: 'preset_speed', name: 'Variable speed', preset: true, cycles: 2, steps: [
        Step(elev: 25, yaw: 0, m1: 3500, m2: 3500, intervalMs: 3000),
        Step(elev: 25, yaw: 0, m1: 4500, m2: 4500, intervalMs: 3000),
        Step(elev: 25, yaw: 0, m1: 5500, m2: 5500, intervalMs: 3000),
        Step(elev: 25, yaw: 0, m1: 4500, m2: 4500, intervalMs: 3000),
      ]),
      Drill(id: 'preset_spin', name: 'Spin variation', preset: true, cycles: 2, steps: [
        Step(elev: 25, yaw: 0, m1: 5000, m2: 5000, spin: -600, shared: false, intervalMs: 4000),
        Step(elev: 25, yaw: 0, m1: 5000, m2: 5000, spin: 0, intervalMs: 4000),
        Step(elev: 25, yaw: 0, m1: 5000, m2: 5000, spin: 600, shared: false, intervalMs: 4000),
      ]),
      Drill(id: 'preset_baseline', name: 'Baseline Variation', preset: true, steps: [
        Step(elev: 25, yaw: 0, m1: 5000, m2: 5000, intervalMs: 3000),
        Step(elev: 35, yaw: -15, m1: 5500, m2: 5500, intervalMs: 4000),
        Step(elev: 20, yaw: 15, m1: 6000, m2: 5800, shared: false, intervalMs: 3000),
        Step(elev: 40, yaw: 0, m1: 5000, m2: 5000, intervalMs: 5000),
      ]),
      Drill(id: 'preset_custom', name: 'Custom sequence (template)', preset: true, steps: [
        Step(elev: 20, yaw: -10, m1: 4000, m2: 4000, intervalMs: 4000),
        Step(elev: 30, yaw: 10, m1: 4500, m2: 4500, intervalMs: 4000),
      ]),
    ];

class DrillStore extends ChangeNotifier {
  final List<Drill> presets = buildPresets();
  final List<Drill> saved = [];
  Future<void> load() async {
    final p = await SharedPreferences.getInstance();
    final raw = p.getString('atlas_drills');
    if (raw != null) {
      try {
        saved
          ..clear()
          ..addAll((jsonDecode(raw) as List).map((e) => Drill.fromJson(Map<String, dynamic>.from(e))));
      } catch (_) {}
    }
  }
  Future<void> _persist() async {
    final p = await SharedPreferences.getInstance();
    await p.setString('atlas_drills', jsonEncode(saved.map((d) => d.toJson()).toList()));
    notifyListeners();
  }
  Future<void> upsert(Drill d) async {
    final i = saved.indexWhere((x) => x.id == d.id);
    if (i >= 0) { saved[i] = d; } else { saved.add(d); }
    await _persist();
  }
  Future<void> delete(Drill d) async { saved.removeWhere((x) => x.id == d.id); await _persist(); }
}

// Local validation mirrors the firmware rules (firmware remains the authority).
List<String> validateDrillLocal(Drill d) {
  final errs = <String>[];
  Map lim = (link.caps['limits'] as Map?) ?? {};
  final maxRpm = _d(lim['max_rpm'], 6000);
  final eMin = _d(lim['elev_min'], 0), eMax = _d(lim['elev_max'], 60);
  final yMin = _d(lim['yaw_min'], -30), yMax = _d(lim['yaw_max'], 30);
  final maxSteps = _i(lim['max_steps'], 40), maxReps = _i(lim['max_reps'], 100);
  final maxCycles = _i(lim['max_cycles'], 50), maxShots = _i(lim['max_total_shots'], 500);
  final feederMin = _i((link.caps['feeder'] as Map?)?['min_interval_ms'], 1300);
  if (d.name.trim().isEmpty) errs.add('Name is empty');
  if (d.id.length > 39) errs.add('Drill id too long');
  if (d.steps.isEmpty) errs.add('Drill has no steps');
  if (d.steps.length > maxSteps) errs.add('Too many steps (max $maxSteps)');
  if (d.cycles < 1 || d.cycles > maxCycles) errs.add('Cycles must be 1..$maxCycles');
  if (d.totalShots > maxShots) errs.add('Total shots ${d.totalShots} exceeds $maxShots');
  for (var i = 0; i < d.steps.length; i++) {
    final s = d.steps[i]; final n = 'Step ${i + 1}';
    if (s.elev < eMin || s.elev > eMax) errs.add('$n: elevation outside ${_fmt(eMin)}..${_fmt(eMax)}°');
    if (s.yaw < yMin || s.yaw > yMax) errs.add('$n: yaw outside ${_fmt(yMin)}..${_fmt(yMax)}°');
    final w1 = s.m1 + s.spin / 2, w2 = s.m2 - s.spin / 2;
    if (w1 < 0 || w2 < 0 || w1 > maxRpm || w2 > maxRpm) errs.add('$n: wheel speeds ${_fmt(w1)}/${_fmt(w2)} outside 0..${_fmt(maxRpm)} RPM');
    if (s.feed && w1 <= 0 && w2 <= 0) errs.add('$n: feeding with wheels at 0 RPM');
    if (s.intervalMs < (s.feed ? feederMin : 100)) errs.add('$n: interval below minimum ${s.feed ? feederMin : 100} ms');
    if (s.intervalMs > 600000) errs.add('$n: interval above 600000 ms');
    if (s.reps < 1 || s.reps > maxReps) errs.add('$n: reps must be 1..$maxReps');
    if (s.requirePos && link.caps['positioning'] != true) errs.add('$n: requires positioning but no positioning hardware');
  }
  return errs;
}

// ============================================================================
// LINK TO ESP32 (WebSocket)
// ============================================================================
enum LinkState { disconnected, connecting, awaitingCaps, ready }

class AtlasLink extends ChangeNotifier {
  String host = '192.168.4.1';
  int port = 81;
  LinkState state = LinkState.disconnected;
  bool wantConnected = false;
  Map<String, dynamic> caps = {}, tel = {}, cfg = {};
  int cfgVersion = 0;
  String? uploadedId, uploadedName;
  final List<String> logLines = [];

  WebSocketChannel? _ch;
  StreamSubscription? _sub;
  Timer? _hb, _retry;
  DateTime _lastTel = DateTime.now();
  int _nextId = 1;
  final Map<int, Completer<Map<String, dynamic>>> _pending = {};

  bool get ready => state == LinkState.ready;
  Map<String, dynamic> sub(String k) => tel[k] is Map ? Map<String, dynamic>.from(tel[k]) : {};
  String get machineState => (tel['state'] ?? '—').toString();
  Map get limits => (caps['limits'] as Map?) ?? {};
  double lim(String k, double d) => _d(limits[k], d);
  bool get hasRpm => caps['rpm_sensors'] == true;
  bool get hasPos => caps['positioning'] == true;
  bool get drillActive {
    final s = sub('drill')['state'];
    return s == 'RUNNING' || s == 'PAUSED';
  }

  void _log(String s) {
    final t = DateTime.now().toIso8601String().substring(11, 19);
    logLines.insert(0, '$t  $s');
    if (logLines.length > 300) logLines.removeLast();
  }
  void refresh() => notifyListeners();
  void _setState(LinkState s) { state = s; notifyListeners(); }
  Map<String, dynamic> _err(String code, String msg) => {'ok': false, 'error': {'code': code, 'message': msg}};

  Future<void> connect() async {
    wantConnected = true;
    _retry?.cancel();
    await _open();
  }

  Future<void> _open() async {
    if (state != LinkState.disconnected) return;
    _setState(LinkState.connecting);
    WebSocketChannel? ch;
    try {
      ch = WebSocketChannel.connect(Uri.parse('ws://$host:$port'));
      await ch.ready.timeout(const Duration(seconds: 5));
      _ch = ch;
      _sub = ch.stream.listen(_onData, onDone: _onClosed, onError: (_) => _onClosed());
      _lastTel = DateTime.now();
      _setState(LinkState.awaitingCaps);
      _hb = Timer.periodic(const Duration(seconds: 1), (_) => _heartbeat());
      final r = await send('get_capabilities');
      if (r['ok'] == true) {
        caps = Map<String, dynamic>.from(r['caps']);
        await refreshConfig();
        await send('get_status');
        _setState(LinkState.ready);
        snack('Connected to ATLAS (fw ${caps['fw']})');
      } else {
        _log('capability request failed: ${r['error']}');
        _teardown();
        _scheduleRetry();
      }
    } catch (e) {
      _log('connect failed: $e');
      try { ch?.sink.close(); } catch (_) {}
      _teardown();
      _scheduleRetry();
    }
  }

  Future<void> refreshConfig() async {
    final r = await send('get_config');
    if (r['ok'] == true) {
      cfg = Map<String, dynamic>.from(r['config']);
      cfgVersion++;
      notifyListeners();
    }
  }

  void _scheduleRetry() {
    if (!wantConnected) return;
    _retry?.cancel();
    _retry = Timer(const Duration(seconds: 3), _open);
  }

  void _heartbeat() {
    if (DateTime.now().difference(_lastTel) > const Duration(seconds: 3)) {
      snack('Connection to ATLAS lost (no telemetry)', error: true);
      _log('telemetry timeout');
      _teardown();
      _scheduleRetry();
      return;
    }
    try { _ch?.sink.add(jsonEncode({'type': 'ping', 'id': 0})); } catch (_) {}
  }

  void _onClosed() {
    if (state == LinkState.disconnected) return;
    snack('Connection to ATLAS lost', error: true);
    _log('socket closed');
    _teardown();
    _scheduleRetry();
  }

  void _teardown() {
    _hb?.cancel(); _sub?.cancel();
    try { _ch?.sink.close(); } catch (_) {}
    _ch = null; _sub = null; _hb = null;
    for (final c in _pending.values) {
      if (!c.isCompleted) c.complete(_err('not_connected', 'Connection closed'));
    }
    _pending.clear();
    tel = {};
    state = LinkState.disconnected;
    notifyListeners();
  }

  void disconnect() {
    wantConnected = false;
    _retry?.cancel();
    _teardown();
  }

  void _onData(dynamic data) {
    Map<String, dynamic> m;
    try { m = Map<String, dynamic>.from(jsonDecode(data as String)); } catch (_) { return; }
    switch (m['type']) {
      case 'telemetry':
        tel = m; _lastTel = DateTime.now(); notifyListeners();
        break;
      case 'ack':
        final id = _i(m['id']);
        if (id != 0) {
          _pending.remove(id)?.complete(m);
          if (m['cmd'] != 'get_status') {
            _log('ACK ${m['cmd']} ${m['ok'] == true ? 'ok' : 'FAILED ${m['error']?['code']}'}');
          }
        }
        break;
      case 'event':
        _log('EVENT ${m['event']} ${m['detail'] ?? m['n'] ?? ''}');
        final e = m['event'];
        if (e == 'fault') snack('FAULT: ${m['fault']} – ${m['detail']}', error: true);
        if (e == 'drill_complete') snack('Drill complete (${m['shots']} shots commanded)');
        if (e == 'drill_failed') snack('Drill failed: ${m['detail']}', error: true);
        if (e == 'drill_cancelled') snack('Drill stopped: ${m['detail']}');
        if (e == 'feeding_stopped') snack('Timed feeding stopped: ${m['detail']}', error: true);
        notifyListeners();
        break;
    }
  }

  Future<Map<String, dynamic>> send(String type, [Map<String, dynamic> params = const {}]) async {
    final ch = _ch;
    if (ch == null || state == LinkState.disconnected || state == LinkState.connecting) {
      return _err('not_connected', 'Not connected to ATLAS');
    }
    final id = _nextId++;
    final c = Completer<Map<String, dynamic>>();
    _pending[id] = c;
    final txt = jsonEncode({'type': type, 'id': id, ...params});
    if (type != 'get_status') _log('TX ${txt.length > 160 ? '${txt.substring(0, 160)}…' : txt}');
    ch.sink.add(txt);
    return c.future.timeout(const Duration(seconds: 5), onTimeout: () {
      _pending.remove(id);
      return _err('timeout', 'No acknowledgement for $type');
    });
  }

  /// Sends a command and reports the acknowledgement to the user. Returns true on success.
  Future<bool> cmd(String label, String type, [Map<String, dynamic> params = const {}]) async {
    final r = await send(type, params);
    if (r['ok'] == true) {
      final w = r['warning'];
      snack(w != null ? '$label: OK – $w' : '$label: OK');
      return true;
    }
    final e = (r['error'] as Map?) ?? {};
    snack('$label failed: ${e['code']} – ${e['message']}', error: true);
    return false;
  }
}

// ============================================================================
// REUSABLE WIDGETS
// ============================================================================
class NumSlider extends StatefulWidget {
  final String label, unit;
  final double value, min, max, step;
  final int decimals;
  final bool enabled;
  final ValueChanged<double> onChanged;
  const NumSlider({
    super.key, required this.label, required this.value, required this.min, required this.max,
    required this.onChanged, this.step = 1, this.unit = '', this.decimals = 0, this.enabled = true,
  });
  @override
  State<NumSlider> createState() => _NumSliderState();
}

class _NumSliderState extends State<NumSlider> {
  late final TextEditingController c = TextEditingController(text: _fmt(widget.value, widget.decimals));
  final FocusNode focus = FocusNode();
  @override
  void didUpdateWidget(NumSlider o) {
    super.didUpdateWidget(o);
    if (!focus.hasFocus && double.tryParse(c.text) != widget.value) c.text = _fmt(widget.value, widget.decimals);
  }
  @override
  void dispose() { c.dispose(); focus.dispose(); super.dispose(); }

  void _set(double v) => widget.onChanged(double.parse(v.clamp(widget.min, widget.max).toStringAsFixed(widget.decimals + 1)));

  @override
  Widget build(BuildContext context) {
    final v = widget.value.clamp(widget.min, widget.max).toDouble();
    return Column(children: [
      Row(children: [
        Expanded(child: Text(widget.label)),
        SizedBox(
          width: 110,
          child: TextField(
            controller: c, focusNode: focus, enabled: widget.enabled,
            keyboardType: const TextInputType.numberWithOptions(decimal: true, signed: true),
            decoration: InputDecoration(isDense: true, suffixText: widget.unit, border: const OutlineInputBorder()),
            onChanged: (s) {
              final x = double.tryParse(s);
              if (x != null && x >= widget.min && x <= widget.max) widget.onChanged(x);
            },
            onSubmitted: (s) {
              final x = double.tryParse(s);
              if (x == null) { c.text = _fmt(widget.value, widget.decimals); } else { _set(x); c.text = _fmt(x.clamp(widget.min, widget.max).toDouble(), widget.decimals); }
            },
          ),
        ),
      ]),
      Row(children: [
        IconButton(visualDensity: VisualDensity.compact, icon: const Icon(Icons.remove_circle_outline), onPressed: widget.enabled ? () => _set(v - widget.step) : null),
        Expanded(
          child: Slider(
            value: v, min: widget.min, max: widget.max,
            onChanged: widget.enabled ? (x) => _set((x / widget.step).round() * widget.step) : null,
          ),
        ),
        IconButton(visualDensity: VisualDensity.compact, icon: const Icon(Icons.add_circle_outline), onPressed: widget.enabled ? () => _set(v + widget.step) : null),
      ]),
    ]);
  }
}

class Section extends StatelessWidget {
  final String title;
  final List<Widget> children;
  const Section(this.title, this.children, {super.key});
  @override
  Widget build(BuildContext context) => Card(
        margin: const EdgeInsets.fromLTRB(12, 8, 12, 4),
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(title, style: Theme.of(context).textTheme.titleMedium?.copyWith(fontWeight: FontWeight.bold)),
            const SizedBox(height: 8),
            ...children,
          ]),
        ),
      );
}

Widget note(String t, {Color? color}) => Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Text(t, style: TextStyle(fontSize: 12.5, color: color ?? Colors.white70)),
    );

// ============================================================================
// APP SHELL
// ============================================================================
Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await store.load();
  runApp(const AtlasApp());
}

class AtlasApp extends StatelessWidget {
  const AtlasApp({super.key});
  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'ATLAS',
        debugShowCheckedModeBanner: false,
        scaffoldMessengerKey: messengerKey,
        theme: ThemeData(useMaterial3: true, colorScheme: ColorScheme.fromSeed(seedColor: Colors.deepOrange, brightness: Brightness.dark)),
        home: const HomeShell(),
      );
}

class HomeShell extends StatefulWidget {
  const HomeShell({super.key});
  @override
  State<HomeShell> createState() => _HomeShellState();
}

class _HomeShellState extends State<HomeShell> {
  int idx = 0;
  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: link,
      builder: (context, _) {
        final st = link.machineState;
        final bad = st == 'FAULT' || st == 'EMERGENCY_STOP';
        return Scaffold(
          appBar: AppBar(
            title: Row(children: [
              const Text('ATLAS'),
              const SizedBox(width: 12),
              Chip(
                visualDensity: VisualDensity.compact,
                backgroundColor: !link.ready ? Colors.grey.shade800 : (bad ? Colors.red.shade900 : Colors.green.shade900),
                label: Text(link.ready ? st : link.state.name, style: const TextStyle(fontSize: 11)),
              ),
            ]),
            actions: [
              Padding(
                padding: const EdgeInsets.only(right: 8),
                child: FilledButton.icon(
                  style: FilledButton.styleFrom(backgroundColor: Colors.red.shade700, foregroundColor: Colors.white),
                  icon: const Icon(Icons.stop_circle),
                  label: const Text('SW STOP'),
                  onPressed: () => link.cmd('Software stop', 'emergency_stop_request'),
                ),
              ),
            ],
          ),
          body: IndexedStack(index: idx, children: const [ConnectPage(), ControlPage(), DrillsPage(), ConfigPage(), LogPage()]),
          bottomNavigationBar: NavigationBar(
            selectedIndex: idx,
            onDestinationSelected: (i) => setState(() => idx = i),
            destinations: const [
              NavigationDestination(icon: Icon(Icons.wifi), label: 'Connect'),
              NavigationDestination(icon: Icon(Icons.tune), label: 'Control'),
              NavigationDestination(icon: Icon(Icons.list_alt), label: 'Drills'),
              NavigationDestination(icon: Icon(Icons.settings), label: 'Config'),
              NavigationDestination(icon: Icon(Icons.terminal), label: 'Log'),
            ],
          ),
        );
      },
    );
  }
}

// ============================================================================
// CONNECT PAGE
// ============================================================================
class ConnectPage extends StatefulWidget {
  const ConnectPage({super.key});
  @override
  State<ConnectPage> createState() => _ConnectPageState();
}

class _ConnectPageState extends State<ConnectPage> {
  final hostC = TextEditingController(text: link.host);
  final portC = TextEditingController(text: link.port.toString());

  Widget kv(String k, String v, {Color? c}) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 2),
        child: Row(children: [Expanded(child: Text(k)), Text(v, style: TextStyle(color: c, fontWeight: FontWeight.w600))]),
      );

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: link,
      builder: (context, _) {
        final f = (link.caps['feeder'] as Map?) ?? {};
        final fault = link.tel['fault'] ?? 'NONE';
        return ListView(padding: const EdgeInsets.only(bottom: 24), children: [
          Section('How to connect', [
            note('1. Power ATLAS (USB during development).\n'
                '2. Android Settings → Wi-Fi → join "ATLAS-Launcher" (password in firmware, default "atlas-launch-1").\n'
                '3. Android will warn that this network has no internet. Choose "Stay connected" / "Keep Wi-Fi on". '
                'Turn mobile data OFF while using ATLAS, otherwise Android may route traffic away from the Wi-Fi.\n'
                '4. Keep host 192.168.4.1, port 81, tap Connect. No internet is needed.'),
          ]),
          Section('Connection', [
            Row(children: [
              Expanded(flex: 3, child: TextField(controller: hostC, decoration: const InputDecoration(labelText: 'ESP32 IP', isDense: true, border: OutlineInputBorder()))),
              const SizedBox(width: 8),
              Expanded(child: TextField(controller: portC, keyboardType: TextInputType.number, decoration: const InputDecoration(labelText: 'Port', isDense: true, border: OutlineInputBorder()))),
            ]),
            const SizedBox(height: 8),
            Row(children: [
              FilledButton(
                onPressed: link.state == LinkState.disconnected
                    ? () { link.host = hostC.text.trim(); link.port = int.tryParse(portC.text) ?? 81; link.connect(); }
                    : null,
                child: const Text('Connect'),
              ),
              const SizedBox(width: 8),
              OutlinedButton(onPressed: link.wantConnected || link.state != LinkState.disconnected ? link.disconnect : null, child: const Text('Disconnect')),
              const Spacer(),
              Text(link.state.name),
            ]),
            if (link.wantConnected && link.state == LinkState.disconnected) note('Reconnecting… (nothing resumes automatically after a reconnect)'),
          ]),
          if (link.ready)
            Section('Machine', [
              kv('Firmware / protocol', '${link.caps['fw']} / ${link.caps['protocol']}'),
              kv('Machine state', link.machineState),
              kv('Fault', '$fault', c: fault == 'NONE' ? Colors.greenAccent : Colors.redAccent),
              if (fault != 'NONE') ...[
                note('${link.tel['fault_msg']}', color: Colors.redAccent),
                FilledButton(onPressed: () => link.cmd('Clear fault', 'clear_fault'), child: const Text('Clear fault (does not restart anything)')),
              ],
              const Divider(),
              kv('RPM sensors', link.hasRpm ? 'installed (closed loop)' : 'NONE – open-loop, RPM not measured', c: link.hasRpm ? Colors.greenAccent : Colors.orangeAccent),
              kv('Angle / yaw positioning', link.hasPos ? 'installed' : 'NONE – angles stored only', c: link.hasPos ? Colors.greenAccent : Colors.orangeAccent),
              kv('Feeder servo', f['calibrated'] == true ? 'calibrated' : 'NOT calibrated – feeding disabled', c: f['calibrated'] == true ? Colors.greenAccent : Colors.orangeAccent),
              kv('Ball sensor', link.caps['ball_sensor'] == true ? 'installed' : 'none (feeds are commanded, not confirmed)'),
              kv('E-stop status input', link.caps['estop_sense'] == true ? 'monitored' : 'not wired'),
              kv('Interlock input', link.caps['interlock'] == true ? 'monitored' : 'not wired'),
              kv('Max RPM (soft / hard)', '${_fmt(link.lim('max_rpm', 0))} / ${_fmt(link.lim('hard_max_rpm', 0))}'),
            ]),
          Section('Safety', [
            note('The "SW STOP" button only disables ESP32 outputs. It is NOT an emergency stop. '
                'Use the physical DC-rated emergency stop to remove motor power.', color: Colors.orangeAccent),
          ]),
        ]);
      },
    );
  }
}

// ============================================================================
// CONTROL PAGE (wheels, angle, feeder)
// ============================================================================
class ControlPage extends StatefulWidget {
  const ControlPage({super.key});
  @override
  State<ControlPage> createState() => _ControlPageState();
}

class _ControlPageState extends State<ControlPage> {
  bool inited = false, sync = true, useLevel = false;
  double rpm = 3000, diff = 0, level = 50, m1 = 3000, m2 = 3000, elev = 25, yaw = 0;
  final intervalC = TextEditingController(text: '3000');
  final bpmC = TextEditingController(text: '20.0');

  void initFromTel() {
    if (inited || link.tel.isEmpty) return;
    final w = link.sub('wheels'), a = link.sub('angle');
    sync = w['mode'] != 'independent';
    m1 = _d(w['req1']); m2 = _d(w['req2']);
    rpm = _d(w['base'], m1); diff = _d(w['diff']);
    elev = _d(a['req_elev'], 25); yaw = _d(a['req_yaw']);
    final iv = _i(link.sub('feeder')['interval_ms'], 3000);
    intervalC.text = '$iv'; bpmC.text = (60000 / iv).toStringAsFixed(1);
    inited = true;
  }

  double levelToRpm(double l) {
    final mn = _d(link.cfg['level_min_rpm'], 1500), mx = _d(link.cfg['level_max_rpm'], 6000);
    return l <= 0 ? 0 : mn + (mx - mn) * l / 100;
  }
  double rpmToLevel(double r) {
    final mn = _d(link.cfg['level_min_rpm'], 1500), mx = _d(link.cfg['level_max_rpm'], 6000);
    return (mx > mn && r > 0) ? ((r - mn) / (mx - mn) * 100).clamp(0, 100).toDouble() : 0;
  }

  Future<void> applySpeed() async {
    final mx = link.lim('max_rpm', 6000);
    if (sync) {
      final base = useLevel ? levelToRpm(level) : rpm;
      if (base + diff.abs() / 2 > mx || base - diff.abs() / 2 < 0) {
        snack('Speed ± differential exceeds 0..${_fmt(mx)} RPM', error: true);
        return;
      }
      await link.cmd('Set launch speed', 'set_launch_speed', useLevel ? {'level_pct': level, 'diff_rpm': diff} : {'rpm': rpm, 'diff_rpm': diff});
    } else {
      await link.cmd('Set wheel speeds', 'set_wheel_speeds', {'m1': m1, 'm2': m2});
    }
  }

  void syncIntervalFromBpm() {
    final b = double.tryParse(bpmC.text);
    if (b != null && b > 0) intervalC.text = (60000 / b).round().toString();
  }
  void syncBpmFromInterval() {
    final v = double.tryParse(intervalC.text);
    if (v != null && v > 0) bpmC.text = (60000 / v).toStringAsFixed(2);
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: link,
      builder: (context, _) {
        initFromTel();
        final en = link.ready;
        final w = link.sub('wheels'), a = link.sub('angle'), f = link.sub('feeder');
        final mx = link.lim('max_rpm', 6000);
        final measured = w['measured'] == true;
        final fMin = _i(f['min_interval_ms'], 1300);
        return ListView(padding: const EdgeInsets.only(bottom: 24), children: [
          if (!en) Section('Not connected', [note('Connect to ATLAS first (Connect tab). Controls are disabled until the machine has answered.')]),
          Section('Launch speed (wheels)', [
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: Text(sync ? 'Synchronized wheels' : 'Independent wheels'),
              subtitle: const Text('Synchronized: one launch speed ± differential (spin). Wheel 1 = upper, wheel 2 = lower.'),
              value: sync,
              onChanged: en && !link.drillActive
                  ? (v) async {
                      final ok = await link.cmd('Wheel mode', 'set_wheel_mode', {'mode': v ? 'synchronized' : 'independent'});
                      if (ok) setState(() => sync = v);
                    }
                  : null,
            ),
            if (sync) ...[
              NumSlider(label: 'Launch speed', unit: 'RPM', value: rpm, min: 0, max: mx, step: 50, enabled: en, onChanged: (v) => setState(() { rpm = v; useLevel = false; level = rpmToLevel(v); })),
              NumSlider(label: 'Launch level (mapped to RPM by config)', unit: '%', value: level, min: 0, max: 100, step: 1, enabled: en, onChanged: (v) => setState(() { level = v; useLevel = true; rpm = levelToRpm(v); })),
              NumSlider(label: 'Differential (W1 − W2, spin)', unit: 'RPM', value: diff, min: -mx / 2, max: mx / 2, step: 50, enabled: en, onChanged: (v) => setState(() => diff = v)),
              note('Result: W1 ${_fmt((useLevel ? levelToRpm(level) : rpm) + diff / 2)} RPM, W2 ${_fmt((useLevel ? levelToRpm(level) : rpm) - diff / 2)} RPM. '
                  'Positive differential = upper wheel faster (assumed topspin).'),
            ] else ...[
              NumSlider(label: 'Wheel 1 (upper) target', unit: 'RPM', value: m1, min: 0, max: mx, step: 50, enabled: en, onChanged: (v) => setState(() => m1 = v)),
              NumSlider(label: 'Wheel 2 (lower) target', unit: 'RPM', value: m2, min: 0, max: mx, step: 50, enabled: en, onChanged: (v) => setState(() => m2 = v)),
            ],
            Wrap(spacing: 8, children: [
              FilledButton(onPressed: en ? applySpeed : null, child: const Text('Set speed')),
              FilledButton.tonal(
                onPressed: en
                    ? () async {
                        if (await confirm(context, 'Start wheels?', 'Wheels will spin up to the targets with the configured ramp. Keep the area clear and the physical E-stop within reach.')) {
                          await link.cmd('Start launch', 'start_launch');
                        }
                      }
                    : null,
                child: const Text('Start launch'),
              ),
              FilledButton.tonal(onPressed: en ? () => link.cmd('Stop launch', 'stop_launch') : null, child: const Text('Stop launch')),
            ]),
            const Divider(),
            Text('Setpoint now (ramped): ${_fmt(_d(w['set1']))} / ${_fmt(_d(w['set2']))} RPM   PWM ${_fmt(_d(w['pwm1']), 1)} / ${_fmt(_d(w['pwm2']), 1)} %'),
            if (measured)
              Text('MEASURED: ${_fmt(_d(w['rpm1']))} / ${_fmt(_d(w['rpm2']))} RPM')
            else
              note('OPEN-LOOP: no RPM sensors – actual wheel speed is NOT measured. Setpoint is an estimate from a PWM model.', color: Colors.orangeAccent),
            Text('Ready: ${w['ready'] == true ? 'yes' : 'no'}  (${w['ready_basis'] ?? '-'})'),
            if (w['surf1_mps'] != null) Text('Wheel surface speed: ${w['surf1_mps']} / ${w['surf2_mps']} m/s'),
            if (w['est_ball_kmh'] != null) Text('Calibrated ball-speed estimate: ${w['est_ball_kmh']} km/h'),
            note('Wheel RPM is not ball exit speed: it also depends on wheel diameter, ball compression, slip and geometry. '
                'No ball speed is claimed unless calibrated (config: kmh_per_rpm).'),
          ]),
          Section('Elevation and yaw', [
            NumSlider(label: 'Elevation (0° = horizontal, + = up)', unit: '°', decimals: 1, value: elev, min: link.lim('elev_min', 0), max: link.lim('elev_max', 60), step: 0.5, enabled: en, onChanged: (v) => setState(() => elev = v)),
            NumSlider(label: 'Yaw (0° = straight, + = right)', unit: '°', decimals: 1, value: yaw, min: link.lim('yaw_min', -30), max: link.lim('yaw_max', 30), step: 0.5, enabled: en, onChanged: (v) => setState(() => yaw = v)),
            Wrap(spacing: 8, children: [
              FilledButton(onPressed: en ? () => link.cmd('Apply position', 'apply_position', {'elev': elev, 'yaw': yaw}) : null, child: const Text('Apply position')),
              OutlinedButton(onPressed: en ? () => link.cmd('Set elevation', 'set_launch_angle', {'elev': elev}) : null, child: const Text('Set elevation only')),
              OutlinedButton(onPressed: en ? () => link.cmd('Set yaw', 'set_yaw_angle', {'yaw': yaw}) : null, child: const Text('Set yaw only')),
            ]),
            const Divider(),
            Text('Requested: elev ${a['req_elev'] ?? '-'}°, yaw ${a['req_yaw'] ?? '-'}°'),
            Text('Actual (measured): ${a['act_elev'] == null ? 'unavailable' : '${a['act_elev']}° / ${a['act_yaw']}°'}'),
            if (a['physical'] != true)
              note('NO positioning hardware: angles are stored/sent and saved in drills, but ATLAS cannot move to them. '
                  'Aim manually. A requested angle is never a measured angle.', color: Colors.orangeAccent)
            else
              Text('Position reached: ${a['reached'] == true ? 'yes' : 'no'}'),
          ]),
          Section('Ball feeder', [
            Text('Feeder: ${f['state'] ?? '-'}   calibrated: ${f['calibrated'] == true ? 'yes' : 'NO'}   feeds commanded: ${f['feeds'] ?? 0}'),
            if (f['calibrated'] != true) note('Feeder servo not calibrated – set servo_rest / servo_feed / servo_min / servo_max in the Config tab first.', color: Colors.orangeAccent),
            Row(children: [
              Expanded(child: TextField(controller: intervalC, keyboardType: TextInputType.number, decoration: InputDecoration(labelText: 'Interval (ms) ≥ $fMin', isDense: true, border: const OutlineInputBorder()), onChanged: (_) => syncBpmFromInterval())),
              const SizedBox(width: 8),
              Expanded(child: TextField(controller: bpmC, keyboardType: const TextInputType.numberWithOptions(decimal: true), decoration: const InputDecoration(labelText: 'Balls / min', isDense: true, border: OutlineInputBorder()), onChanged: (_) => syncIntervalFromBpm())),
            ]),
            note('interval_ms = 60000 / balls_per_minute'),
            Wrap(spacing: 6, children: [
              for (final s in [2, 3, 5]) ActionChip(label: Text('${s}s'), onPressed: () { intervalC.text = '${s * 1000}'; syncBpmFromInterval(); }),
            ]),
            Wrap(spacing: 8, children: [
              OutlinedButton(onPressed: en ? () => link.cmd('Feed interval', 'set_feed_interval', {'interval_ms': int.tryParse(intervalC.text) ?? 0}) : null, child: const Text('Apply interval')),
              OutlinedButton(onPressed: en ? () => link.cmd('Feed rate', 'set_feed_rate', {'balls_per_min': double.tryParse(bpmC.text) ?? 0}) : null, child: const Text('Apply rate')),
            ]),
            Wrap(spacing: 8, children: [
              FilledButton(onPressed: en ? () => link.cmd('Feed one ball', 'feed_one_ball') : null, child: const Text('Feed one ball')),
              FilledButton.tonal(onPressed: en ? () => link.cmd('Start feeding', 'start_feeding', {'interval_ms': int.tryParse(intervalC.text) ?? 0}) : null, child: const Text('Start feeding')),
              FilledButton.tonal(onPressed: en ? () => link.cmd('Pause feeding', 'pause_feeding') : null, child: const Text('Pause')),
              FilledButton.tonal(onPressed: en ? () => link.cmd('Resume feeding', 'resume_feeding') : null, child: const Text('Resume')),
              FilledButton.tonal(onPressed: en ? () => link.cmd('Stop feeding', 'stop_feeding') : null, child: const Text('Stop')),
            ]),
            Text('Timed feeding: ${f['auto'] == true ? (f['paused'] == true ? 'paused' : 'running') : 'off'}, interval ${f['interval_ms'] ?? '-'} ms'),
            note('Feeding requires stable wheels and state READY_TO_LAUNCH. Counts are commanded feed events, not confirmed launches (no ball sensor). '
                'During a drill the step interval takes precedence and the global interval is locked.'),
          ]),
        ]);
      },
    );
  }
}

// ============================================================================
// DRILLS PAGE
// ============================================================================
Future<bool> uploadDrill(BuildContext context, Drill d, {bool validateOnly = false}) async {
  final errs = validateDrillLocal(d);
  if (errs.isNotEmpty) {
    await messageDialog(context, 'Drill not valid', errs.join('\n'));
    return false;
  }
  final r = await link.send(validateOnly ? 'validate_drill' : 'upload_drill', {'drill': d.toJson()});
  if (!context.mounted) return false;
  if (r['ok'] == true) {
    final w = ((r['warnings'] as List?) ?? []).join('\n\n');
    if (!validateOnly) { link.uploadedId = d.id; link.uploadedName = d.name; link.refresh(); }
    await messageDialog(context, validateOnly ? 'Drill valid' : 'Drill uploaded (not started)',
        '${r['steps']} steps, ${r['total_shots']} shots.${w.isEmpty ? '' : '\n\nNotes:\n$w'}');
    return true;
  }
  final e = (r['error'] as Map?) ?? {};
  await messageDialog(context, 'ESP32 rejected the drill', '${e['code']}\n${e['message']}');
  return false;
}

class DrillsPage extends StatelessWidget {
  const DrillsPage({super.key});

  Future<void> openEditor(BuildContext context, Drill d, {bool isNew = false}) async {
    final working = d.preset ? d.copy(id: newId(), name: '${d.name} (copy)', preset: false) : d.copy();
    await Navigator.push(context, MaterialPageRoute(builder: (_) => DrillEditorPage(drill: working)));
  }

  Widget tile(BuildContext context, Drill d) {
    final dur = Duration(milliseconds: d.estMs);
    return Card(
      margin: const EdgeInsets.fromLTRB(12, 4, 12, 4),
      child: ListTile(
        title: Text(d.name),
        subtitle: Text('${d.steps.length} steps × ${d.cycles} cycle(s), ${d.totalShots} shots, ≈${dur.inMinutes}m ${dur.inSeconds % 60}s${link.uploadedId == d.id ? '\n● loaded on ATLAS' : ''}'),
        isThreeLine: link.uploadedId == d.id,
        onTap: () => openEditor(context, d),
        trailing: PopupMenuButton<String>(
          onSelected: (v) async {
            switch (v) {
              case 'edit': await openEditor(context, d); break;
              case 'upload': await uploadDrill(context, d); break;
              case 'dup': await store.upsert(d.copy(id: newId(), name: '${d.name} (copy)', preset: false)); break;
              case 'rename':
                final c = TextEditingController(text: d.name);
                final n = await showDialog<String>(context: context, builder: (_) => AlertDialog(title: const Text('Rename drill'), content: TextField(controller: c, autofocus: true), actions: [TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')), FilledButton(onPressed: () => Navigator.pop(context, c.text), child: const Text('OK'))]));
                if (n != null && n.trim().isNotEmpty) { d.name = n.trim(); await store.upsert(d); }
                break;
              case 'del':
                if (await confirm(context, 'Delete drill?', d.name)) await store.delete(d);
                break;
            }
          },
          itemBuilder: (_) => [
            const PopupMenuItem(value: 'upload', child: Text('Upload to ATLAS')),
            PopupMenuItem(value: 'edit', child: Text(d.preset ? 'Edit a copy' : 'Edit')),
            const PopupMenuItem(value: 'dup', child: Text('Duplicate')),
            if (!d.preset) const PopupMenuItem(value: 'rename', child: Text('Rename')),
            if (!d.preset) const PopupMenuItem(value: 'del', child: Text('Delete')),
          ],
        ),
      ),
    );
  }

  Widget runPanel(BuildContext context) {
    final d = link.sub('drill');
    final st = (d['state'] ?? 'NONE').toString();
    final active = st == 'RUNNING' || st == 'PAUSED';
    final en = link.ready;
    final loaded = st != 'NONE';
    return Section('Drill execution (ESP32 owns the timing)', [
      Text(loaded ? '${d['name']}  [$st]' : 'No drill loaded on ATLAS (upload one below)'),
      if (loaded) ...[
        const SizedBox(height: 6),
        LinearProgressIndicator(value: _d(d['progress']).clamp(0, 1).toDouble()),
        Text('Step ${d['step']}/${d['steps']}  rep ${d['rep']}/${d['reps']}  cycle ${d['cycle']}/${d['cycles']}  shots ${d['shots']}/${d['total_shots']}  phase ${d['phase']}'),
        if (d['pause_pending'] == true) note('Pause pending (finishes current feed cycle)'),
        if (d['aim_not_performed'] == true) note('Elevation/yaw were NOT physically applied (no positioning hardware). Aim manually.', color: Colors.orangeAccent),
        if (d['error'] != null) note('Error: ${d['error']}', color: Colors.redAccent),
        if (st == 'DONE') note('Complete. Feed events were commanded, not confirmed launches.'),
      ],
      Wrap(spacing: 8, children: [
        FilledButton(
          onPressed: en && loaded && !active
              ? () async {
                  if (await confirm(context, 'Start drill?', 'ATLAS will spin the wheels and feed balls automatically on its own timing. Clear the area; keep the physical E-stop within reach.')) {
                    await link.cmd('Start drill', 'start_drill', {'id': d['id']});
                  }
                }
              : null,
          child: const Text('Start'),
        ),
        FilledButton.tonal(onPressed: en && st == 'RUNNING' ? () => link.cmd('Pause drill', 'pause_drill') : null, child: const Text('Pause')),
        FilledButton.tonal(onPressed: en && st == 'PAUSED' ? () => link.cmd('Resume drill', 'resume_drill') : null, child: const Text('Resume')),
        FilledButton.tonal(onPressed: en && active ? () => link.cmd('Stop drill', 'stop_drill') : null, child: const Text('Stop')),
        OutlinedButton(onPressed: en ? () => link.cmd('Drill status', 'get_drill_status') : null, child: const Text('Refresh')),
      ]),
      note('Uploading never starts a drill. A drill is cancelled if the phone disconnects and is never resumed automatically. The drill is held in ESP32 RAM and lost on reboot.'),
    ]);
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([link, store]),
      builder: (context, _) => Scaffold(
        floatingActionButton: FloatingActionButton.extended(
          icon: const Icon(Icons.add),
          label: const Text('New drill'),
          onPressed: () => openEditor(context, Drill(id: newId(), name: 'New drill', steps: [Step()])),
        ),
        body: ListView(padding: const EdgeInsets.only(bottom: 90), children: [
          runPanel(context),
          const Padding(padding: EdgeInsets.fromLTRB(16, 12, 16, 4), child: Text('Pre-programmed (editing makes a copy)', style: TextStyle(fontWeight: FontWeight.bold))),
          ...store.presets.map((d) => tile(context, d)),
          const Padding(padding: EdgeInsets.fromLTRB(16, 12, 16, 4), child: Text('My drills (stored on this phone)', style: TextStyle(fontWeight: FontWeight.bold))),
          if (store.saved.isEmpty) note('  None yet. Tap "New drill" or edit a preset.'),
          ...store.saved.map((d) => tile(context, d)),
        ]),
      ),
    );
  }
}

// ============================================================================
// DRILL EDITOR
// ============================================================================
class DrillEditorPage extends StatefulWidget {
  final Drill drill;
  const DrillEditorPage({super.key, required this.drill});
  @override
  State<DrillEditorPage> createState() => _DrillEditorPageState();
}

class _DrillEditorPageState extends State<DrillEditorPage> {
  late Drill d = widget.drill;
  late final nameC = TextEditingController(text: d.name);
  bool dirty = false;

  Future<void> editStep(int i) async {
    final r = await showModalBottomSheet<Step>(
      context: context, isScrollControlled: true,
      builder: (_) => StepSheet(step: d.steps[i].copy(), index: i),
    );
    if (r != null) setState(() { d.steps[i] = r; dirty = true; });
  }

  Future<void> save() async {
    d.name = nameC.text.trim().isEmpty ? 'Drill' : nameC.text.trim();
    await store.upsert(d);
    dirty = false;
    snack('Saved "${d.name}" on this phone');
    if (mounted) setState(() {});
  }

  void preview() {
    var t = 0;
    final rows = <String>[];
    for (var c = 0; c < d.cycles; c++) {
      for (var i = 0; i < d.steps.length; i++) {
        final s = d.steps[i];
        rows.add('t≈${(t / 1000).toStringAsFixed(0)}s  cycle ${c + 1} step ${i + 1}: ${s.summary()}${s.feed ? '' : '  (no feed)'}${s.spin != 0 ? '  spin ${_fmt(s.spin)}' : ''}');
        t += s.intervalMs * s.reps + s.pauseAfterMs;
      }
      t += d.cyclePauseMs;
    }
    messageDialog(context, 'Preview – ${d.totalShots} shots, ≈${(d.estMs / 1000).toStringAsFixed(0)} s',
        '${rows.join('\n\n')}\n\nEstimate excludes wheel spin-up, positioning and feeder time. ${link.hasPos ? '' : 'No positioning hardware: angles are NOT physically applied.'}');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(dirty ? 'Edit drill *' : 'Edit drill'),
        actions: [
          IconButton(tooltip: 'Preview', icon: const Icon(Icons.visibility), onPressed: preview),
          IconButton(tooltip: 'Validate', icon: const Icon(Icons.rule), onPressed: () async {
            d.name = nameC.text.trim();
            final errs = validateDrillLocal(d);
            if (errs.isNotEmpty) { await messageDialog(context, 'Drill not valid', errs.join('\n')); return; }
            if (link.ready) { await uploadDrill(context, d, validateOnly: true); } else { snack('Locally valid (connect to validate on ATLAS)'); }
          }),
          IconButton(tooltip: 'Upload to ATLAS', icon: const Icon(Icons.upload), onPressed: () { d.name = nameC.text.trim(); uploadDrill(context, d); }),
          IconButton(tooltip: 'Save', icon: const Icon(Icons.save), onPressed: save),
        ],
      ),
      body: ReorderableListView.builder(
        buildDefaultDragHandles: false,
        padding: const EdgeInsets.only(bottom: 24),
        header: Padding(
          padding: const EdgeInsets.all(12),
          child: Column(children: [
            TextField(controller: nameC, onChanged: (_) => dirty = true, decoration: const InputDecoration(labelText: 'Drill name', border: OutlineInputBorder())),
            NumSlider(label: 'Cycles (repeat whole sequence)', value: d.cycles.toDouble(), min: 1, max: link.lim('max_cycles', 50), onChanged: (v) => setState(() { d.cycles = v.round(); dirty = true; })),
            NumSlider(label: 'Pause between cycles', unit: 'ms', value: d.cyclePauseMs.toDouble(), min: 0, max: 60000, step: 500, onChanged: (v) => setState(() { d.cyclePauseMs = v.round(); dirty = true; })),
            Text('${d.steps.length} steps · ${d.totalShots} total shots'),
          ]),
        ),
        footer: Padding(
          padding: const EdgeInsets.all(12),
          child: FilledButton.icon(
            icon: const Icon(Icons.add),
            label: const Text('Add step'),
            onPressed: () => setState(() { d.steps.add(d.steps.isNotEmpty ? d.steps.last.copy() : Step()); dirty = true; }),
          ),
        ),
        itemCount: d.steps.length,
        onReorder: (o, n) => setState(() {
          if (n > o) n--;
          d.steps.insert(n, d.steps.removeAt(o));
          dirty = true;
        }),
        itemBuilder: (_, i) {
          final s = d.steps[i];
          return Card(
            key: ObjectKey(s),
            margin: const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
            child: ListTile(
              onTap: () => editStep(i),
              title: Text('Step ${i + 1}'),
              subtitle: Text('${s.summary()}${s.feed ? '' : '\n(no feed)'}${s.spin != 0 ? '\nspin ${_fmt(s.spin)} RPM' : ''}'),
              trailing: Row(mainAxisSize: MainAxisSize.min, children: [
                IconButton(tooltip: 'Duplicate', icon: const Icon(Icons.copy), onPressed: () => setState(() { d.steps.insert(i + 1, s.copy()); dirty = true; })),
                IconButton(tooltip: 'Delete', icon: const Icon(Icons.delete), onPressed: d.steps.length > 1 ? () => setState(() { d.steps.removeAt(i); dirty = true; }) : null),
                ReorderableDragStartListener(index: i, child: const Padding(padding: EdgeInsets.all(8), child: Icon(Icons.drag_handle))),
              ]),
            ),
          );
        },
      ),
    );
  }
}

class StepSheet extends StatefulWidget {
  final Step step;
  final int index;
  const StepSheet({super.key, required this.step, required this.index});
  @override
  State<StepSheet> createState() => _StepSheetState();
}

class _StepSheetState extends State<StepSheet> {
  late Step s = widget.step;
  late final bpmC = TextEditingController(text: (60000 / s.intervalMs).toStringAsFixed(2));

  @override
  Widget build(BuildContext context) {
    final mx = link.lim('max_rpm', 6000);
    final feederMin = _i((link.caps['feeder'] as Map?)?['min_interval_ms'], 1300);
    return Padding(
      padding: EdgeInsets.only(bottom: MediaQuery.of(context).viewInsets.bottom),
      child: SingleChildScrollView(
        padding: const EdgeInsets.all(16),
        child: Column(children: [
          Text('Step ${widget.index + 1}', style: Theme.of(context).textTheme.titleLarge),
          NumSlider(label: 'Elevation', unit: '°', decimals: 1, step: 0.5, value: s.elev, min: link.lim('elev_min', 0), max: link.lim('elev_max', 60), onChanged: (v) => setState(() => s.elev = v)),
          NumSlider(label: 'Yaw', unit: '°', decimals: 1, step: 0.5, value: s.yaw, min: link.lim('yaw_min', -30), max: link.lim('yaw_max', 30), onChanged: (v) => setState(() => s.yaw = v)),
          SwitchListTile(contentPadding: EdgeInsets.zero, title: const Text('Shared speed (both wheels equal)'), value: s.shared, onChanged: (v) => setState(() { s.shared = v; if (v) s.m2 = s.m1; })),
          NumSlider(label: s.shared ? 'Launch speed' : 'Wheel 1 RPM', unit: 'RPM', step: 50, value: s.m1, min: 0, max: mx, onChanged: (v) => setState(() { s.m1 = v; if (s.shared) s.m2 = v; })),
          if (!s.shared) NumSlider(label: 'Wheel 2 RPM', unit: 'RPM', step: 50, value: s.m2, min: 0, max: mx, onChanged: (v) => setState(() => s.m2 = v)),
          NumSlider(label: 'Spin (W1 +½, W2 −½)', unit: 'RPM', step: 50, value: s.spin, min: -mx / 2, max: mx / 2, onChanged: (v) => setState(() => s.spin = v)),
          NumSlider(label: 'Time between launches', unit: 'ms', step: 100, value: s.intervalMs.toDouble(), min: 100, max: 60000, onChanged: (v) => setState(() { s.intervalMs = v.round(); bpmC.text = (60000 / s.intervalMs).toStringAsFixed(2); })),
          Row(children: [
            const Expanded(child: Text('…or feed rate (balls/min)')),
            SizedBox(width: 110, child: TextField(controller: bpmC, keyboardType: const TextInputType.numberWithOptions(decimal: true), decoration: const InputDecoration(isDense: true, border: OutlineInputBorder()), onChanged: (t) { final b = double.tryParse(t); if (b != null && b > 0) setState(() => s.intervalMs = (60000 / b).round().clamp(100, 600000)); })),
          ]),
          if (s.feed && s.intervalMs < feederMin) note('Below feeder minimum cycle ($feederMin ms): ATLAS will reject this.', color: Colors.redAccent),
          NumSlider(label: 'Repetitions of this step', value: s.reps.toDouble(), min: 1, max: link.lim('max_reps', 100), onChanged: (v) => setState(() => s.reps = v.round())),
          NumSlider(label: 'Pause after step (added to interval)', unit: 'ms', step: 100, value: s.pauseAfterMs.toDouble(), min: 0, max: 60000, onChanged: (v) => setState(() => s.pauseAfterMs = v.round())),
          SwitchListTile(contentPadding: EdgeInsets.zero, title: const Text('Feed balls in this step'), value: s.feed, onChanged: (v) => setState(() => s.feed = v)),
          SwitchListTile(contentPadding: EdgeInsets.zero, title: const Text('Wait for stable wheel speed'), subtitle: Text(link.hasRpm ? 'Measured RPM within tolerance' : 'Open-loop: time-based assumption, not measured'), value: s.waitStable, onChanged: (v) => setState(() => s.waitStable = v)),
          SwitchListTile(contentPadding: EdgeInsets.zero, title: const Text('Require positioning reached'), subtitle: Text(link.hasPos ? 'Waits for actuators' : 'Unavailable: no positioning hardware (ATLAS will reject)'), value: s.requirePos, onChanged: (v) => setState(() => s.requirePos = v)),
          SwitchListTile(contentPadding: EdgeInsets.zero, title: const Text('Pause drill after this step'), subtitle: const Text('Wait for Resume before the next step'), value: s.holdAfter, onChanged: (v) => setState(() => s.holdAfter = v)),
          Row(children: [
            TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
            const Spacer(),
            OutlinedButton(
              onPressed: () async {
                final test = Drill(id: 'test_${DateTime.now().millisecondsSinceEpoch % 100000000}', name: 'Test step', steps: [s.copy()..reps = 1]);
                if (!link.ready) { snack('Connect to ATLAS first', error: true); return; }
                if (!await confirm(context, 'Test one shot?', 'Uploads this step as a one-shot drill and starts it: wheels spin up and one ball is fed. Clear the area.')) return;
                if (!context.mounted) return;
                if (await uploadDrill(context, test)) await link.cmd('Start test shot', 'start_drill', {'id': test.id});
              },
              child: const Text('Test this step'),
            ),
            const SizedBox(width: 8),
            FilledButton(onPressed: () => Navigator.pop(context, s), child: const Text('Done')),
          ]),
        ]),
      ),
    );
  }
}

// ============================================================================
// CONFIG PAGE (limits, ramps, mapping, feeder calibration)
// ============================================================================
class CfgField {
  final String key, label, unit;
  const CfgField(this.key, this.label, [this.unit = '']);
}

const List<(String, List<CfgField>)> cfgGroups = [
  ('Wheel limits and ramps', [
    CfgField('max_rpm', 'Maximum permitted RPM (soft)', 'RPM'),
    CfgField('max_pwm_pct', 'Maximum PWM duty', '%'),
    CfgField('ramp_up_rpm_s', 'Acceleration ramp', 'RPM/s'),
    CfgField('ramp_down_rpm_s', 'Deceleration ramp', 'RPM/s'),
    CfgField('rpm_at_full_pwm', 'Open-loop model: RPM at 100% PWM (calibrate!)', 'RPM'),
  ]),
  ('Launch level and calibration', [
    CfgField('level_min_rpm', 'Level mapping: RPM at lowest level', 'RPM'),
    CfgField('level_max_rpm', 'Level mapping: RPM at 100%', 'RPM'),
    CfgField('wheel_dia_mm', 'Wheel diameter (0 = unknown)', 'mm'),
    CfgField('kmh_per_rpm', 'Experimental km/h per RPM (0 = uncalibrated)'),
  ]),
  ('Speed readiness / closed loop', [
    CfgField('settle_ms', 'Open-loop assumed settle time', 'ms'),
    CfgField('stable_ms', 'Closed-loop stable time', 'ms'),
    CfgField('stable_tol_pct', 'Stability tolerance', '%'),
    CfgField('pulses_per_rev', 'RPM sensor pulses per revolution'),
    CfgField('kp', 'PID Kp'), CfgField('ki', 'PID Ki'), CfgField('kd', 'PID Kd'),
  ]),
  ('Angle soft limits', [
    CfgField('elev_min', 'Elevation min', '°'), CfgField('elev_max', 'Elevation max', '°'),
    CfgField('yaw_min', 'Yaw min', '°'), CfgField('yaw_max', 'Yaw max', '°'),
  ]),
  ('Feeder servo (REQUIRED before feeding)', [
    CfgField('servo_min', 'Servo travel limit min', '°'), CfgField('servo_max', 'Servo travel limit max', '°'),
    CfgField('servo_rest', 'Rest position', '°'), CfgField('servo_feed', 'Release (feed) position', '°'),
    CfgField('servo_offset', 'Calibration offset', '°'),
    CfgField('servo_pulse_min_us', 'Servo pulse min', 'µs'), CfgField('servo_pulse_max_us', 'Servo pulse max', 'µs'),
    CfgField('feed_move_ms', 'Time to reach release position', 'ms'),
    CfgField('feed_hold_ms', 'Time held at release position', 'ms'),
    CfgField('feed_return_ms', 'Time to return to rest', 'ms'),
    CfgField('feed_timeout_ms', 'Feed cycle timeout (fault)', 'ms'),
    CfgField('feed_interval_ms', 'Default feed interval', 'ms'),
    CfgField('max_feed_interval_ms', 'Maximum feed interval', 'ms'),
  ]),
  ('Safety timing', [
    CfgField('ready_timeout_ms', 'Max wait for readiness (fault)', 'ms'),
    CfgField('comm_timeout_ms', 'Stop if phone silent for', 'ms'),
  ]),
];

class ConfigPage extends StatefulWidget {
  const ConfigPage({super.key});
  @override
  State<ConfigPage> createState() => _ConfigPageState();
}

class _ConfigPageState extends State<ConfigPage> {
  final Map<String, TextEditingController> ctl = {};
  int loadedVersion = -1;

  void syncControllers() {
    if (loadedVersion == link.cfgVersion) return;
    loadedVersion = link.cfgVersion;
    for (final g in cfgGroups) {
      for (final f in g.$2) {
        final c = ctl.putIfAbsent(f.key, () => TextEditingController());
        final v = link.cfg[f.key];
        c.text = v == null ? '' : (v is num ? (v == v.roundToDouble() ? v.toInt().toString() : v.toString()) : '$v');
      }
    }
  }

  Future<void> apply() async {
    final vals = <String, dynamic>{};
    for (final e in ctl.entries) {
      final cur = link.cfg[e.key];
      final v = double.tryParse(e.value.text);
      if (v == null) { snack('${e.key}: not a number', error: true); return; }
      if (cur is! num || (cur.toDouble() - v).abs() > 1e-9) vals[e.key] = v;
    }
    if (vals.isEmpty) { snack('No changes'); return; }
    if (await link.cmd('Apply config (${vals.length})', 'set_config', {'values': vals})) await link.refreshConfig();
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: link,
      builder: (context, _) {
        syncControllers();
        final en = link.ready;
        return ListView(padding: const EdgeInsets.only(bottom: 24), children: [
          Section('Machine configuration', [
            note('Applies to ATLAS RAM. Press "Save to flash" to keep values after reboot. Changes are refused while wheels, feeder or a drill are active.'),
            Wrap(spacing: 8, children: [
              FilledButton(onPressed: en ? apply : null, child: const Text('Apply changes')),
              FilledButton.tonal(onPressed: en ? () => link.cmd('Save config', 'save_config') : null, child: const Text('Save to flash')),
              OutlinedButton(onPressed: en ? link.refreshConfig : null, child: const Text('Reload')),
            ]),
            note('Before operating the servo you must know: servo model and supply voltage/peak current, mechanical end stops (safe min/max angle), '
                'rest and release angles, time to move/hold/return, and that the servo supply ground is common with the ESP32. '
                'Defaults are LOCKED (no motion) until you enter real values. These are never guessed by the software.', color: Colors.orangeAccent),
          ]),
          for (final g in cfgGroups)
            Section(g.$1, [
              for (final f in g.$2)
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 4),
                  child: TextField(
                    controller: ctl[f.key], enabled: en,
                    keyboardType: const TextInputType.numberWithOptions(decimal: true, signed: true),
                    decoration: InputDecoration(labelText: f.label, suffixText: f.unit, isDense: true, border: const OutlineInputBorder()),
                  ),
                ),
            ]),
        ]);
      },
    );
  }
}

// ============================================================================
// LOG PAGE
// ============================================================================
class LogPage extends StatelessWidget {
  const LogPage({super.key});
  @override
  Widget build(BuildContext context) => ListenableBuilder(
        listenable: link,
        builder: (context, _) => ListView.builder(
          padding: const EdgeInsets.all(8),
          itemCount: link.logLines.length,
          itemBuilder: (_, i) => Text(link.logLines[i], style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
        ),
      );
}
