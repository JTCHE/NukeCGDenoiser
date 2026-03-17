import nuke

if nuke.GUI:
    nuke.menu('Nodes').addMenu('MW')
    nuke.menu('Nodes').addCommand('MW/Denoiser', lambda: nuke.createNode('Denoiser'))

nuke.load('denoiser')