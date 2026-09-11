local M={}
function M.new(deps)
    local self={isOpen=false,disposed=false}
    function self:show(page)
        if self.disposed then return end
        deps.changePage(page)
        if not self.isOpen then
            self.isOpen=true
            deps.openPanel()
        end
    end
    function self:closed()
        self.isOpen=false
        deps.leavePage()
    end
    function self:dispose()
        self.disposed=true
        self:closed()
    end
    return self
end
return M
