import { createSignal, For, Show } from "solid-js";
import type { TodoList, Todoist } from "../proto/dashboard";
import { send } from "../connection";
import { dueLabel } from "../format";
import { Card, Empty } from "./Card";
import { Icon } from "./Icon";
import { t } from "../i18n";

const STORAGE_KEY = "dashboard.todoList";

function loadSelected(): string | null {
  try {
    return localStorage.getItem(STORAGE_KEY);
  } catch {
    return null;
  }
}

export function Tasks(props: { todo?: Todoist; now: number }) {
  // Remember the chosen list per device (phone and desktop can differ).
  const [selected, setSelected] = createSignal<string | null>(loadSelected());
  // Hide completed tasks immediately; the next snapshot confirms it.
  const [done, setDone] = createSignal<Set<string>>(new Set());

  const lists = () => props.todo?.lists ?? [];
  const openTasks = (l: TodoList) =>
    l.task.filter((task) => !done().has(`${l.entityId}/${task.uid}`));
  const current = () => lists().find((l) => l.entityId === selected()) ?? lists()[0];
  const tasks = () => {
    const l = current();
    return l ? openTasks(l) : [];
  };

  const choose = (entityId: string) => {
    setSelected(entityId);
    try {
      localStorage.setItem(STORAGE_KEY, entityId);
    } catch {
      /* storage unavailable: selection just isn't remembered */
    }
  };

  const complete = (uid: string) => {
    const entityId = current()?.entityId;
    if (!entityId) return;
    if (send({ completeTask: { uid, entityId } })) setDone((s) => new Set(s).add(`${entityId}/${uid}`));
  };

  return (
    <Card
      title={t("tasks")}
      class="tall tasks-card"
      trailing={
        <Show when={tasks().length > 0}>
          <span class="badge label-medium">{tasks().length}</span>
        </Show>
      }
    >
      <Show when={lists().length > 0} fallback={<Empty>{t("noTodo")}</Empty>}>
        <Show when={lists().length > 1}>
          <div class="filter-chips" role="tablist" aria-label={t("todoList")}>
            <For each={lists()}>
              {(l) => {
                const active = () => l.entityId === current()?.entityId;
                return (
                  <button
                    role="tab"
                    class="filter-chip label-large"
                    classList={{ selected: active() }}
                    aria-selected={active()}
                    onClick={() => choose(l.entityId)}
                  >
                    <Show when={active()}>
                      <Icon name="check" size={18} />
                    </Show>
                    {l.name}
                    <span class="filter-chip-count">{openTasks(l).length}</span>
                  </button>
                );
              }}
            </For>
          </div>
        </Show>

        <Show
          when={tasks().length > 0}
          fallback={
            <div class="all-done">
              <Icon name="task_alt" size={40} />
              <span class="body-large">{t("allDone")}</span>
            </div>
          }
        >
          <ul class="list">
            <For each={tasks()}>
              {(task) => {
                const due = () => (task.due ? dueLabel(task.due, props.now) : undefined);
                return (
                  <li class="list-item">
                    <button class="checkbox" onClick={() => complete(task.uid)} aria-label={t("completeTask")(task.summary)}>
                      <span class="checkbox-box">
                        <Icon name="check" size={16} />
                      </span>
                    </button>
                    <div class="list-text">
                      <div class="body-large">{task.summary}</div>
                      <Show when={due()}>
                        {(d) => (
                          <div class={`due body-medium ${d().state}`}>
                            <Icon name="schedule" size={16} />
                            {d().text}
                          </div>
                        )}
                      </Show>
                    </div>
                  </li>
                );
              }}
            </For>
          </ul>
        </Show>
      </Show>
    </Card>
  );
}
