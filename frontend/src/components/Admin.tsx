import { createResource, createSignal, For, Show } from "solid-js";
import { api, ApiError } from "../api";
import { me } from "../auth";
import { errorText, permText, t } from "../i18n";
import { Dialog, Switch, TextField } from "./Controls";
import { Icon } from "./Icon";

interface UserRow {
  name: string;
  admin: boolean;
  permissions: string[];
  disabled: boolean;
  created: number;
}
interface UsersResponse {
  users: UserRow[];
  permissions: string[];
}

const DEFAULT_PERMS = ["weather", "lights"];

/** Admin screen: create users, set what they may see/control, disable, delete. */
export function Admin(props: { onBack: () => void }) {
  const [data, { mutate, refetch }] = createResource(() => api<UsersResponse>("GET", "/api/users"));
  const [message, setMessage] = createSignal<{ text: string; error: boolean } | null>(null);
  let msgTimer: number | undefined;

  const flash = (text: string, error = false) => {
    setMessage({ text, error });
    clearTimeout(msgTimer);
    msgTimer = window.setTimeout(() => setMessage(null), 3500);
  };
  const fail = (err: unknown) => flash(err instanceof ApiError ? errorText(err.code) : errorText("network"), true);

  const replace = (u: UserRow) =>
    mutate((d) => d && { ...d, users: d.users.map((x) => (x.name === u.name ? u : x)) });

  const update = async (name: string, patch: Partial<UserRow> & { password?: string }) => {
    try {
      replace(await api<UserRow>("PUT", `/api/users/${encodeURIComponent(name)}`, patch));
      flash(t("saved"));
      return true;
    } catch (err) {
      fail(err);
      void refetch(); // undo optimistic UI
      return false;
    }
  };

  return (
    <div class="admin">
      <header class="admin-header">
        <button class="icon-button large" aria-label={t("back")} onClick={props.onBack}>
          <Icon name="arrow_back" size={24} />
        </button>
        <h1 class="headline-small">{t("manageUsers")}</h1>
      </header>

      <CreateUser
        perms={data()?.permissions ?? []}
        onCreated={(u) => {
          mutate((d) => d && { ...d, users: [...d.users, u] });
          flash(t("saved"));
        }}
        onError={fail}
      />

      <Show when={data()} fallback={<div class="spinner" />}>
        {(d) => (
          <div class="user-list">
            <For each={d().users}>
              {(u) => (
                <UserCard
                  user={u}
                  perms={d().permissions}
                  self={u.name === me()?.name}
                  onUpdate={(patch) => update(u.name, patch)}
                  onDeleted={() => {
                    mutate((x) => x && { ...x, users: x.users.filter((y) => y.name !== u.name) });
                    flash(t("saved"));
                  }}
                  onError={fail}
                />
              )}
            </For>
          </div>
        )}
      </Show>

      <Show when={message()}>
        {(m) => (
          <div class="snackbar body-medium" classList={{ error: m().error }} role="status">
            {m().text}
          </div>
        )}
      </Show>
    </div>
  );
}

function PermChips(props: { all: string[]; selected: string[]; disabled?: boolean; onToggle: (p: string) => void }) {
  return (
    <div class="perm-chips">
      <For each={props.all}>
        {(p) => {
          const on = () => props.selected.includes(p);
          return (
            <button
              type="button"
              class="filter-chip label-large"
              classList={{ selected: on() }}
              aria-pressed={on()}
              disabled={props.disabled}
              onClick={() => props.onToggle(p)}
            >
              <Show when={on()}>
                <Icon name="check" size={18} />
              </Show>
              {permText(p)}
            </button>
          );
        }}
      </For>
    </div>
  );
}

const toggle = (list: string[], p: string) => (list.includes(p) ? list.filter((x) => x !== p) : [...list, p]);

function CreateUser(props: { perms: string[]; onCreated: (u: UserRow) => void; onError: (e: unknown) => void }) {
  const [open, setOpen] = createSignal(false);
  const [name, setName] = createSignal("");
  const [password, setPassword] = createSignal("");
  const [perms, setPerms] = createSignal<string[]>(DEFAULT_PERMS);
  const [admin, setAdmin] = createSignal(false);
  const [busy, setBusy] = createSignal(false);

  const reset = () => {
    setName("");
    setPassword("");
    setPerms(DEFAULT_PERMS);
    setAdmin(false);
    setOpen(false);
  };

  const submit = async (e: Event) => {
    e.preventDefault();
    setBusy(true);
    try {
      props.onCreated(
        await api<UserRow>("POST", "/api/users", { username: name(), password: password(), admin: admin(), permissions: perms() }),
      );
      reset();
    } catch (err) {
      props.onError(err);
    } finally {
      setBusy(false);
    }
  };

  return (
    <Show
      when={open()}
      fallback={
        <button class="tonal-button label-large new-user-button" onClick={() => setOpen(true)}>
          <Icon name="person_add" size={20} />
          {t("newUser")}
        </button>
      }
    >
      <form class="card create-user" onSubmit={submit}>
        <h2 class="title-medium">{t("newUser")}</h2>
        <div class="create-fields">
          <TextField label={t("username")} value={name()} onInput={setName} autocomplete="off" autofocus />
          <TextField
            label={t("password")}
            type="password"
            value={password()}
            onInput={setPassword}
            autocomplete="new-password"
            hint={t("passwordHint")}
          />
        </div>
        <div class="label-large variant">{t("access")}</div>
        <PermChips all={props.perms} selected={admin() ? props.perms : perms()} disabled={admin()} onToggle={(p) => setPerms((l) => toggle(l, p))} />
        <div class="admin-toggle">
          <div>
            <div class="body-large">{t("admin")}</div>
            <div class="body-small variant">{t("adminHint")}</div>
          </div>
          <Switch checked={admin()} onChange={setAdmin} label={t("admin")} />
        </div>
        <div class="dialog-actions">
          <button type="button" class="text-button label-large" onClick={reset}>
            {t("cancel")}
          </button>
          <button type="submit" class="filled-button label-large" disabled={busy() || !name() || password().length < 8}>
            {t("create")}
          </button>
        </div>
      </form>
    </Show>
  );
}

function UserCard(props: {
  user: UserRow;
  perms: string[];
  self: boolean;
  onUpdate: (patch: Partial<UserRow> & { password?: string }) => Promise<boolean>;
  onDeleted: () => void;
  onError: (e: unknown) => void;
}) {
  const [dialog, setDialog] = createSignal<"password" | "delete" | null>(null);
  const [newPw, setNewPw] = createSignal("");
  const u = () => props.user;

  const remove = async () => {
    try {
      await api("DELETE", `/api/users/${encodeURIComponent(u().name)}`);
      setDialog(null);
      props.onDeleted();
    } catch (err) {
      setDialog(null);
      props.onError(err);
    }
  };

  return (
    <section class="card user-card" classList={{ disabled: u().disabled }}>
      <div class="user-head">
        <span class="avatar title-medium" aria-hidden="true">
          {u().name.slice(0, 1).toUpperCase()}
        </span>
        <div class="user-title">
          <div class="title-medium ellipsis">{u().name}</div>
          <div class="user-badges">
            <Show when={u().admin}>
              <span class="badge-chip label-small">
                <Icon name="admin_panel_settings" size={14} />
                {t("admin")}
              </span>
            </Show>
            <Show when={props.self}>
              <span class="badge-chip label-small">{t("you")}</span>
            </Show>
            <Show when={u().disabled}>
              <span class="badge-chip error label-small">{t("disabledUser")}</span>
            </Show>
          </div>
        </div>
        <div class="user-active">
          <span class="label-medium variant">{t("active")}</span>
          <Switch
            checked={!u().disabled}
            disabled={props.self}
            label={t("active")}
            onChange={(v) => void props.onUpdate({ disabled: !v })}
          />
        </div>
      </div>

      <div class="label-large variant">{u().admin ? t("allAccess") : t("access")}</div>
      <PermChips
        all={props.perms}
        selected={u().permissions}
        disabled={u().admin}
        onToggle={(p) => void props.onUpdate({ permissions: toggle(u().permissions, p) })}
      />

      <div class="user-actions">
        <Show when={!props.self}>
          <button class="text-button label-large" onClick={() => void props.onUpdate({ admin: !u().admin })}>
            <Icon name="admin_panel_settings" size={18} />
            {u().admin ? t("removeAdmin") : t("makeAdmin")}
          </button>
        </Show>
        <button class="text-button label-large" onClick={() => (setNewPw(""), setDialog("password"))}>
          <Icon name="key" size={18} />
          {t("resetPassword")}
        </button>
        <Show when={!props.self}>
          <button class="text-button danger label-large" onClick={() => setDialog("delete")}>
            <Icon name="delete" size={18} />
            {t("deleteUser")}
          </button>
        </Show>
      </div>

      <Show when={dialog() === "password"}>
        <Dialog
          title={t("resetPasswordFor")(u().name)}
          onClose={() => setDialog(null)}
          actions={
            <>
              <button class="text-button label-large" onClick={() => setDialog(null)}>
                {t("cancel")}
              </button>
              <button
                class="filled-button label-large"
                disabled={newPw().length < 8}
                onClick={async () => {
                  if (await props.onUpdate({ password: newPw() })) setDialog(null);
                }}
              >
                {t("save")}
              </button>
            </>
          }
        >
          <TextField label={t("newPassword")} type="password" value={newPw()} onInput={setNewPw} autocomplete="new-password" hint={t("passwordHint")} autofocus />
        </Dialog>
      </Show>

      <Show when={dialog() === "delete"}>
        <Dialog
          title={t("deleteUser")}
          onClose={() => setDialog(null)}
          actions={
            <>
              <button class="text-button label-large" onClick={() => setDialog(null)}>
                {t("cancel")}
              </button>
              <button class="filled-button danger label-large" onClick={remove}>
                {t("delete")}
              </button>
            </>
          }
        >
          <p class="body-medium">{t("confirmDelete")(u().name)}</p>
        </Dialog>
      </Show>
    </section>
  );
}
