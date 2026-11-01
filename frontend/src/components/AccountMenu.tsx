import { createSignal, onCleanup, onMount, Show } from "solid-js";
import { api, ApiError } from "../api";
import { logout, me } from "../auth";
import { errorText, t } from "../i18n";
import { Dialog, TextField } from "./Controls";
import { Icon } from "./Icon";

export function AccountMenu(props: { onManageUsers: () => void }) {
  const [open, setOpen] = createSignal(false);
  const [pwDialog, setPwDialog] = createSignal(false);
  let root: HTMLDivElement | undefined;

  onMount(() => {
    const close = (e: MouseEvent) => {
      if (root && !root.contains(e.target as Node)) setOpen(false);
    };
    document.addEventListener("click", close);
    onCleanup(() => document.removeEventListener("click", close));
  });

  return (
    <div class="account" ref={root}>
      <button class="account-button label-large" aria-haspopup="menu" aria-expanded={open()} onClick={() => setOpen((o) => !o)}>
        <Icon name="account_circle" size={20} />
        <span class="ellipsis">{me()?.name}</span>
      </button>
      <Show when={open()}>
        <div class="menu" role="menu">
          <div class="menu-header">
            <div class="title-small">{me()?.name}</div>
            <div class="body-small variant">{me()?.admin ? t("admin") : t("account")}</div>
          </div>
          <Show when={me()?.admin}>
            <button role="menuitem" class="menu-item body-large" onClick={() => (setOpen(false), props.onManageUsers())}>
              <Icon name="manage_accounts" size={20} />
              {t("manageUsers")}
            </button>
          </Show>
          <button role="menuitem" class="menu-item body-large" onClick={() => (setOpen(false), setPwDialog(true))}>
            <Icon name="key" size={20} />
            {t("changePassword")}
          </button>
          <button role="menuitem" class="menu-item body-large" onClick={() => (setOpen(false), void logout())}>
            <Icon name="logout" size={20} />
            {t("logout")}
          </button>
        </div>
      </Show>
      <Show when={pwDialog()}>
        <PasswordDialog onClose={() => setPwDialog(false)} />
      </Show>
    </div>
  );
}

function PasswordDialog(props: { onClose: () => void }) {
  const [current, setCurrent] = createSignal("");
  const [next, setNext] = createSignal("");
  const [repeat, setRepeat] = createSignal("");
  const [error, setError] = createSignal("");
  const [busy, setBusy] = createSignal(false);

  const save = async () => {
    if (next().length < 8) return setError(errorText("weak_password"));
    if (next() !== repeat()) return setError(errorText("mismatch"));
    setBusy(true);
    setError("");
    try {
      await api("POST", "/api/me/password", { current: current(), new: next() });
      props.onClose();
    } catch (err) {
      setError(err instanceof ApiError ? errorText(err.code) : errorText("network"));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Dialog
      title={t("changePassword")}
      onClose={props.onClose}
      actions={
        <>
          <button class="text-button label-large" onClick={props.onClose}>
            {t("cancel")}
          </button>
          <button class="filled-button label-large" disabled={busy() || !current() || !next()} onClick={save}>
            {t("save")}
          </button>
        </>
      }
    >
      <TextField label={t("currentPassword")} type="password" value={current()} onInput={setCurrent} autocomplete="current-password" autofocus />
      <TextField label={t("newPassword")} type="password" value={next()} onInput={setNext} autocomplete="new-password" hint={t("passwordHint")} />
      <TextField label={t("repeatPassword")} type="password" value={repeat()} onInput={setRepeat} autocomplete="new-password" />
      <Show when={error()}>
        <p class="form-error body-medium" role="alert">
          <Icon name="error" size={18} />
          {error()}
        </p>
      </Show>
    </Dialog>
  );
}
