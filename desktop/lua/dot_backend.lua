local backend = {}
local channel
local client_id, max_encoded_bytes, generation = ...

for _, info in ipairs(vim.api.nvim_list_chans()) do
  local attributes = info.client and info.client.attributes
  if attributes and attributes.dot_backend == client_id then
    assert(not channel, "Ambiguous dot RPC channel")
    channel = info.id
  end
end
assert(channel, "The dot RPC channel was not found")

local colors = {}
local pending = false
local role_links = {
  Accent = { "Statement" },
  Changed = { "Changed" },
  Error = { "ErrorMsg" },
  Frame = { "TabLine" },
  Information = { "DiagnosticHint" },
  MutedText = { "MiniStatuslineFileinfo", "Comment" },
  Raised = { "Substitute" },
  Resting = { "PmenuSbar" },
  SelectedText = { "Cursor" },
  Selection = { "MiniStatuslineModeVisual", "Visual" },
  Success = { "Added" },
  Text = { "Normal" },
  Warning = { "WarningMsg" },
}
local entered = vim.v.vim_did_enter ~= 0
local group = vim.api.nvim_create_augroup("DotColors", { clear = true })
package.loaded["dot.colors"] = colors

local function current()
  return package.loaded["dot.colors"] == colors
end

function colors.refresh(request_generation)
  assert(current() and request_generation == generation, "Stale dot color request")
  local roles = {}
  for role in pairs(role_links) do
    local name = "Dot" .. role
    local attr = vim.api.nvim_get_hl(0, { create = false, link = false, name = name })
    roles[name] = {
      bg = attr.bg or vim.NIL,
      exists = vim.fn.hlexists(name) == 1,
      fg = attr.fg or vim.NIL,
      reverse = attr.reverse or false,
    }
  end
  vim.rpcnotify(channel, "dot_colors", generation, { background = vim.o.background, roles = roles })
  -- A no-op redraw emits no flush, leaving the snapshot unpublished.
  vim.cmd("redraw!")
  return true
end

local function link_target(candidates)
  if #candidates == 1 then
    return candidates[1]
  end
  local preferred = vim.api.nvim_get_hl(0, { create = false, link = true, name = candidates[1] })
  return next(preferred) and candidates[1] or candidates[2]
end

local function schedule_colors()
  if not entered or pending then
    return
  end
  pending = true
  vim.schedule(function()
    if not current() then
      return
    end
    pending = false
    for role, candidates in pairs(role_links) do
      local default = "DotDefault" .. role
      vim.api.nvim_set_hl(0, default, { link = link_target(candidates) })
      vim.api.nvim_set_hl(0, "Dot" .. role, { default = true, link = default })
    end
    colors.refresh(generation)
  end)
end

vim.api.nvim_create_autocmd("ColorScheme", { callback = schedule_colors, group = group })
vim.api.nvim_create_autocmd("OptionSet", { callback = schedule_colors, group = group, pattern = "background" })
if entered then
  schedule_colors()
else
  vim.api.nvim_create_autocmd("VimEnter", {
    callback = function()
      entered = true
      schedule_colors()
    end,
    group = group,
    once = true,
  })
end

if #vim.api.nvim_get_runtime_file("lua/vim/ui/img.lua", false) == 0 then
  return false
end

local function request(action, ...)
  vim.cmd("redraw")
  return vim.rpcrequest(channel, "dot_image", action, ...)
end

function backend._supported()
  for _, ui in ipairs(vim.api.nvim_list_uis()) do
    if ui.chan == channel then
      return true, "dot native image backend"
    end
  end
  return false
end

function backend.del(id)
  if id == math.huge then
    return request("delete_all")
  end
  return request("delete", id)
end

function backend.get(id)
  local result = request("get", id)
  if result == vim.NIL then
    return nil
  end
  return result
end

function backend.set(data_or_id, opts)
  if type(data_or_id) == "string" and #data_or_id > max_encoded_bytes then
    error(("PNG exceeds the encoded image limit (%d bytes)"):format(max_encoded_bytes), 2)
  end
  return request("set", data_or_id, opts == nil and vim.empty_dict() or opts)
end

vim.ui.img = backend
package.loaded["vim.ui.img"] = backend
return true
