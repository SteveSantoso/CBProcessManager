// app.js  ─  ProcessManager frontend
// Vue 3 (CDN global build) + Element Plus

const { createApp, ref, reactive, computed, onMounted, nextTick } = Vue;

const {
  VideoPlay, VideoPause, Setting, Plus, Edit, Delete,
  FolderOpened, RefreshRight, Remove
} = ElementPlusIconsVue;

// ─── WebView2 bridge ─────────────────────────────────────────────────────────
function postMsg(obj) {
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.postMessage(obj);
  } else {
    // Dev fallback: log to console
    console.log('[→ C++]', JSON.stringify(obj));
  }
}

// ─── App ─────────────────────────────────────────────────────────────────────
const App = {
  template: `
    <header class="pm-header">
      <div class="pm-header__logo">
        <span class="pm-header__icon">⚙</span>
        <span class="pm-header__title">进程管理器</span>
      </div>
      <div class="pm-header__actions">
        <el-tag v-if="runningCount > 0" type="success" effect="dark" round>
          {{ runningCount }} 个运行中
        </el-tag>
        <el-button type="success" :icon="VideoPlay" @click="startAll" :disabled="processes.length === 0">
          全部启动
        </el-button>
        <el-button type="danger" :icon="VideoPause" @click="stopAll" :disabled="runningCount === 0">
          全部停止
        </el-button>
        <el-tooltip content="全局设置" placement="bottom">
          <el-button :icon="Setting" circle @click="openSettings"></el-button>
        </el-tooltip>
      </div>
    </header>

    <main class="pm-main">
      <el-empty v-if="processes.length === 0"
                description="暂无进程，点击右下角 + 按钮添加"
                class="pm-empty">
      </el-empty>
      <el-table v-else
                :data="processes"
                stripe
                class="pm-table"
                :header-cell-style="{ background: 'var(--el-fill-color-light)', fontWeight: '600' }"
                row-key="id">
        <el-table-column label="" width="8" class-name="status-dot-col">
          <template #default="{ row }">
            <span :class="['status-dot', 'status-dot--' + row.status]"></span>
          </template>
        </el-table-column>
        <el-table-column prop="name" label="名称" min-width="130" show-overflow-tooltip>
          <template #default="{ row }">
            <span class="proc-name">{{ row.name || '(未命名)' }}</span>
          </template>
        </el-table-column>
        <el-table-column prop="path" label="路径" min-width="220" show-overflow-tooltip>
          <template #default="{ row }">
            <el-tooltip :content="row.path" placement="top" :show-after="600">
              <span class="proc-path">{{ row.path }}</span>
            </el-tooltip>
          </template>
        </el-table-column>
        <el-table-column label="类型" width="80" align="center">
          <template #default="{ row }">
            <el-tag :type="row.type === 'exe' ? 'primary' : 'warning'" size="small" effect="plain">
              .{{ row.type }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="延迟" width="80" align="center">
          <template #default="{ row }">
            <span v-if="row.delaySeconds > 0" class="delay-badge">{{ row.delaySeconds }}s</span>
            <span v-else class="text-muted">—</span>
          </template>
        </el-table-column>
        <el-table-column label="守护" width="70" align="center">
          <template #default="{ row }">
            <el-tooltip :content="row.guardEnabled ? ('崩溃后 ' + row.guardDelaySeconds + 's 重启') : '未启用'" placement="top">
              <el-icon :class="row.guardEnabled ? 'guard-on' : 'guard-off'">
                <refresh-right v-if="row.guardEnabled"></refresh-right>
                <remove v-else></remove>
              </el-icon>
            </el-tooltip>
          </template>
        </el-table-column>
        <el-table-column label="后台" width="60" align="center">
          <template #default="{ row }">
            <el-tooltip :content="row.background ? '后台运行（无控制台）' : '普通窗口'" placement="top">
              <el-tag v-if="row.background" type="info" size="small" effect="plain">后台</el-tag>
              <span v-else class="text-muted">—</span>
            </el-tooltip>
          </template>
        </el-table-column>
        <el-table-column label="状态" width="110" align="center">
          <template #default="{ row }">
            <el-tag :type="statusType(row.status)" size="small" effect="light" class="status-tag">
              <span :class="['status-dot-sm', 'status-dot--' + row.status]"></span>
              {{ statusLabel(row.status) }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="PID" width="90" align="center">
          <template #default="{ row }">
            <span v-if="row.pid > 0" class="pid-badge">{{ row.pid }}</span>
            <span v-else class="text-muted">—</span>
          </template>
        </el-table-column>
        <el-table-column label="操作" width="170" align="center" fixed="right">
          <template #default="{ row }">
            <el-space :size="4">
              <el-tooltip :content="isRunning(row) ? '停止' : '启动'" placement="top">
                <el-button
                  :type="isRunning(row) ? 'danger' : 'success'"
                  circle size="small"
                  :icon="isRunning(row) ? VideoPause : VideoPlay"
                  :loading="row.status === 'starting' || row.status === 'restarting'"
                  @click="toggleProcess(row)">
                </el-button>
              </el-tooltip>
              <el-tooltip content="编辑" placement="top">
                <el-button type="primary" circle size="small" :icon="Edit" @click="editProcess(row)"></el-button>
              </el-tooltip>
              <el-tooltip content="删除" placement="top">
                <el-button type="danger" circle size="small" :icon="Delete"
                           @click="deleteProcess(row)" :disabled="isRunning(row)">
                </el-button>
              </el-tooltip>
            </el-space>
          </template>
        </el-table-column>
      </el-table>
    </main>

    <el-button class="pm-fab" type="primary" circle size="large"
               :icon="Plus" @click="addProcess">
    </el-button>

    <el-dialog v-model="dialogVisible"
               :title="dialogMode === 'add' ? '添加进程' : '编辑进程'"
               width="560px" destroy-on-close align-center>
      <el-form :model="form" :rules="rules" ref="formRef"
               label-width="90px" label-position="left">
        <el-form-item label="名称" prop="name">
          <el-input v-model="form.name" placeholder="给进程起个名字" clearable></el-input>
        </el-form-item>
        <el-form-item label="路径" prop="path">
          <div class="path-row">
            <el-input v-model="form.path" placeholder="exe 或 bat 文件路径"
                      clearable @input="autoDetectType"></el-input>
            <el-button :icon="FolderOpened" @click="openFilePicker">浏览</el-button>
          </div>
        </el-form-item>
        <el-form-item label="类型">
          <el-radio-group v-model="form.type">
            <el-radio value="exe">.exe 可执行文件</el-radio>
            <el-radio value="bat">.bat / .cmd 脚本</el-radio>
          </el-radio-group>
        </el-form-item>
        <el-form-item label="参数">
          <el-input v-model="form.args" placeholder="启动参数（可选）" clearable></el-input>
        </el-form-item>
        <el-form-item label="延迟启动">
          <el-input-number v-model="form.delaySeconds" :min="0" :max="300"
                           :step="1" controls-position="right">
          </el-input-number>
          <span class="unit-label">秒（0 = 立即启动）</span>
        </el-form-item>
        <el-form-item label="进程守护">
          <el-switch v-model="form.guardEnabled" active-text="启用" inactive-text="关闭"></el-switch>
        </el-form-item>
        <el-form-item v-if="form.guardEnabled" label="守护延迟">
          <el-input-number v-model="form.guardDelaySeconds" :min="0" :max="60"
                           :step="1" controls-position="right">
          </el-input-number>
          <span class="unit-label">秒后重启</span>
        </el-form-item>
        <el-form-item label="启用">
          <el-switch v-model="form.enabled"
                     active-text="包含在全部启动中" inactive-text="跳过">
          </el-switch>
        </el-form-item>
        <el-form-item label="后台进程">
          <el-switch v-model="form.background" active-text="后台运行" inactive-text="普通"></el-switch>
          <div class="setting-hint" style="margin-top:4px;">启用后进程将在后台静默运行，不显示控制台/命令行窗口（适合 Node.js 等服务进程）</div>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="dialogVisible = false">取消</el-button>
        <el-button type="primary" @click="submitForm">
          {{ dialogMode === 'add' ? '添加' : '保存' }}
        </el-button>
      </template>
    </el-dialog>

    <el-dialog v-model="settingsVisible" title="全局设置"
               width="440px" destroy-on-close align-center>
      <el-form label-width="160px" label-position="left">
        <el-form-item label="启动时自动运行">
          <el-switch v-model="config.autoStartOnOpen"
                     active-text="是" inactive-text="否">
          </el-switch>
          <div class="setting-hint">打开软件后自动启动所有已启用的进程</div>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="settingsVisible = false">取消</el-button>
        <el-button type="primary" @click="saveSettings">保存</el-button>
      </template>
    </el-dialog>
  `,
  setup() {
    // ── State ──────────────────────────────────────────────────────────────
    const processes = ref([]);
    const config    = reactive({ autoStartOnOpen: false });

    // Dialog
    const dialogVisible = ref(false);
    const dialogMode    = ref('add');  // 'add' | 'edit'
    const formRef       = ref(null);
    const form          = reactive({
      id:               '',
      name:             '',
      path:             '',
      type:             'exe',
      args:             '',
      delaySeconds:     0,
      guardEnabled:     true,
      guardDelaySeconds:1,
      enabled:          true,
      background:       false,
    });
    const rules = {
      name: [{ required: true, message: '请输入名称', trigger: 'blur' }],
      path: [{ required: true, message: '请选择或输入路径', trigger: 'blur' }],
    };

    const settingsVisible = ref(false);

    // ── Computed ───────────────────────────────────────────────────────────
    const runningCount = computed(() =>
      processes.value.filter(p => p.status === 'running' || p.status === 'restarting').length
    );

    // ── Helpers ────────────────────────────────────────────────────────────
    function statusType(s) {
      const map = { running: 'success', stopped: 'info', starting: 'warning', restarting: '', failed: 'danger' };
      return map[s] ?? 'info';
    }
    function statusLabel(s) {
      const map = { running: '运行中', stopped: '已停止', starting: '启动中', restarting: '重启中', failed: '启动失败' };
      return map[s] ?? s;
    }
    function isRunning(row) {
      return row.status === 'running' || row.status === 'starting' || row.status === 'restarting';
    }
    function autoDetectType() {
      const p = form.path.toLowerCase();
      if (p.endsWith('.bat') || p.endsWith('.cmd')) form.type = 'bat';
      else form.type = 'exe';
    }

    // ── Actions ────────────────────────────────────────────────────────────
    function startAll()  { postMsg({ action: 'startAll' }); }
    function stopAll()   { postMsg({ action: 'stopAll'  }); }

    function toggleProcess(row) {
      if (isRunning(row)) postMsg({ action: 'stopProcess',  id: row.id });
      else                postMsg({ action: 'startProcess', id: row.id });
    }

    function addProcess() {
      dialogMode.value = 'add';
      Object.assign(form, {
        id: '', name: '', path: '', type: 'exe', args: '',
        delaySeconds: 0, guardEnabled: true, guardDelaySeconds: 1, enabled: true, background: false
      });
      dialogVisible.value = true;
    }

    function editProcess(row) {
      dialogMode.value = 'edit';
      Object.assign(form, { ...row });
      dialogVisible.value = true;
    }

    async function deleteProcess(row) {
      try {
        await ElementPlus.ElMessageBox.confirm(
          `确定删除「${row.name || row.path}」吗？`, '删除确认',
          { confirmButtonText: '删除', cancelButtonText: '取消', type: 'warning' }
        );
        postMsg({ action: 'deleteProcess', id: row.id });
      } catch (_) { /* cancelled */ }
    }

    function openFilePicker() {
      postMsg({ action: 'openFilePicker' });
    }

    function submitForm() {
      formRef.value?.validate(valid => {
        if (!valid) return;
        const payload = {
          id:               form.id,
          name:             form.name,
          path:             form.path,
          type:             form.type,
          args:             form.args,
          delaySeconds:     form.delaySeconds,
          guardEnabled:     form.guardEnabled,
          guardDelaySeconds:form.guardDelaySeconds,
          enabled:          form.enabled,
          background:       form.background,
        };
        if (dialogMode.value === 'add') {
          postMsg({ action: 'addProcess', process: payload });
        } else {
          postMsg({ action: 'updateProcess', process: payload });
        }
        dialogVisible.value = false;
      });
    }

    function openSettings() {
      settingsVisible.value = true;
    }

    function saveSettings() {
      postMsg({ action: 'saveConfig', config: { autoStartOnOpen: config.autoStartOnOpen } });
      settingsVisible.value = false;
      ElementPlus.ElMessage.success('设置已保存');
    }

    // ── WebView2 message handler ───────────────────────────────────────────
    function handleMessage(event) {
      const data = typeof event.data === 'string' ? JSON.parse(event.data) : event.data;
      if (!data || !data.type) return;

      switch (data.type) {
        case 'processListResponse':
          processes.value = data.processes || [];
          break;

        case 'processStatusChanged': {
          const idx = processes.value.findIndex(p => p.id === data.id);
          if (idx !== -1) {
            processes.value[idx] = { ...processes.value[idx], status: data.status, pid: data.pid ?? 0 };
            // 重新赋值以触发响应式更新
            processes.value = [...processes.value];
          }
          break;
        }

        case 'filePickerResult':
          form.path = data.path || '';
          form.type = data.fileType || 'exe';
          break;

        case 'configResponse':
          config.autoStartOnOpen = !!data.autoStartOnOpen;
          break;

        default:
          break;
      }
    }

    // ── Mount ──────────────────────────────────────────────────────────────
    onMounted(() => {
      if (window.chrome && window.chrome.webview) {
        window.chrome.webview.addEventListener('message', handleMessage);
      }
      // Request initial data
      postMsg({ action: 'getProcessList' });
      postMsg({ action: 'getConfig' });
    });

    return {
      processes, config,
      dialogVisible, dialogMode, form, formRef, rules,
      settingsVisible,
      runningCount,
      statusType, statusLabel, isRunning, autoDetectType,
      startAll, stopAll, toggleProcess,
      addProcess, editProcess, deleteProcess,
      openFilePicker, submitForm,
      openSettings, saveSettings,
      // Icons
      VideoPlay, VideoPause, Setting, Plus, Edit, Delete,
      FolderOpened, RefreshRight, Remove
    };
  }
};

const app = createApp(App);
app.use(ElementPlus);
for (const [name, comp] of Object.entries(ElementPlusIconsVue)) {
  app.component(name, comp);
}
app.mount('#app');
